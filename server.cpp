#include "communication.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

using namespace spy;
using Clock = std::chrono::steady_clock;

static const int NUM_PLAYERS = 5;
static const int INVESTIGATION_SECS = 60;
static const int DISCUSSION_SECS = 180;
static const int VOTING_SECS = 120;
static const int MAX_TIES = 2;

enum class Phase { LOBBY, INVESTIGATION, DISCUSSION, VOTING };

struct Player {
    std::string name;
    std::string role;
    uid_t uid = 0;
    bool alive = true;
};

static Server srv;
static std::map<int, Player> players;   // fd -> player
static std::map<int, int> votes;        // voter fd -> target fd
static Phase phase = Phase::LOBBY;
static Clock::time_point phase_end;
static int round_no = 0;
static int ties_in_row = 0;

// ---------- helpers ----------
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void info(const std::string& t) { srv.broadcast({"INFO", t}); }
static void err(int fd, const std::string& t) { srv.send_to(fd, {"ERROR", t}); }

static int find_by_name(const std::string& name) {
    for (auto& kv : players)
        if (lower(kv.second.name) == lower(name)) return kv.first;
    return -1;
}

// "Player3", "player3" or just "3" -> fd (or -1)
static int parse_target(const std::string& text) {
    std::istringstream in(text);
    std::string w;
    in >> w;
    if (lower(w) == "/vote" || lower(w) == "/investigate") in >> w;
    if (w.empty()) return -1;
    bool digits = std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); });
    if (digits) w = "Player" + w;
    return find_by_name(w);
}

static int count_alive() {
    int n = 0;
    for (auto& kv : players) if (kv.second.alive) n++;
    return n;
}

static std::string alive_list() {
    std::string s;
    for (auto& kv : players)
        if (kv.second.alive) { if (!s.empty()) s += ", "; s += kv.second.name; }
    return s;
}

static bool all_voted() {
    for (auto& kv : players)
        if (kv.second.alive && !votes.count(kv.first)) return false;
    return true;
}

static bool detective_alive() {
    for (auto& kv : players)
        if (kv.second.alive && kv.second.role == "DETECTIVE") return true;
    return false;
}

// ---------- game flow ----------
static void reset_game() {
    for (auto& kv : players) srv.kick(kv.first);   // disconnect everyone
    players.clear();
    votes.clear();
    phase = Phase::LOBBY;
    round_no = 0;
    ties_in_row = 0;
    printf("reset: waiting for %d players\n", NUM_PLAYERS);
}

static void end_game(const std::string& headline) {
    std::string s = "\n========== GAME OVER ==========\n" + headline + "\n\nRoles:\n";
    for (auto& kv : players)
        s += "  " + kv.second.name + " was " + kv.second.role + "\n";
    s += "\nThanks for playing! The server is resetting.";
    info(s);
    printf("game over: %s\n", headline.c_str());
    reset_game();
}

// Returns true (and ends the game) if someone has won.
static bool check_win() {
    int alive = 0;
    bool spy_alive = false;
    for (auto& kv : players)
        if (kv.second.alive) {
            alive++;
            if (kv.second.role == "SPY") spy_alive = true;
        }
    if (!spy_alive) { end_game("THE SPY HAS BEEN ELIMINATED. AGENTS WIN!"); return true; }
    if (alive <= 2) { end_game("Only 2 players remain. THE SPY WINS!"); return true; }
    return false;
}

static void start_discussion() {
    phase = Phase::DISCUSSION;
    phase_end = Clock::now() + std::chrono::seconds(DISCUSSION_SECS);
    votes.clear();
    info("\n========== ROUND " + std::to_string(round_no) + ": DISCUSSION (" +
         std::to_string(DISCUSSION_SECS) + "s) ==========\n"
         "Remaining players: " + alive_list() + "\n"
         "Type to talk. /private PlayerN message for a secret note.");
    printf("phase: DISCUSSION (round %d)\n", round_no);
}

static void start_investigation() {
    phase = Phase::INVESTIGATION;
    phase_end = Clock::now() + std::chrono::seconds(INVESTIGATION_SECS);
    info("\n========== ROUND " + std::to_string(round_no) + ": INVESTIGATION (" +
         std::to_string(INVESTIGATION_SECS) + "s) ==========\n"
         "Remaining players: " + alive_list() + "\n"
         "The Detective is choosing someone to investigate. Everyone else, please wait.");
    for (auto& kv : players)
        if (kv.second.alive && kv.second.role == "DETECTIVE")
            srv.send_to(kv.first, {"INFO", "DETECTIVE: type the name of the player to investigate, e.g. Player3"});
    printf("phase: INVESTIGATION (round %d)\n", round_no);
}

static void start_round() {
    round_no++;
    if (detective_alive()) start_investigation();
    else start_discussion();
}

static void start_voting() {
    phase = Phase::VOTING;
    phase_end = Clock::now() + std::chrono::seconds(VOTING_SECS);
    votes.clear();
    info("\n========== VOTING (" + std::to_string(VOTING_SECS) + "s) ==========\n"
         "Remaining players: " + alive_list() + "\n"
         "Type the name of the player you suspect, e.g. Player3. Your vote is final.\n"
         "Voting ends when everyone has voted.");
    printf("phase: VOTING\n");
}

static void tally_votes() {
    std::map<int, int> count;
    std::string out = "\n========== RESULTS ==========\n";
    for (auto& kv : players) {
        if (!kv.second.alive) continue;
        auto it = votes.find(kv.first);
        if (it == votes.end() || !players[it->second].alive) {
            out += kv.second.name + " -> (no vote)\n";
        } else {
            out += kv.second.name + " -> " + players[it->second].name + "\n";
            count[it->second]++;
        }
    }
    int best = 0;
    std::vector<int> top;
    for (auto& c : count) {
        if (c.second > best) { best = c.second; top.assign(1, c.first); }
        else if (c.second == best) top.push_back(c.first);
    }
    votes.clear();

    if (top.size() != 1) {                       // tie or nobody voted
        ties_in_row++;
        out += "\nTie (or no votes). Nobody is eliminated.";
        info(out);
        printf("result: tie (%d in a row)\n", ties_in_row);
        if (ties_in_row >= MAX_TIES) {
            end_game("The vote failed twice in a row. THE SPY WINS!");
            return;
        }
        start_round();
        return;
    }

    ties_in_row = 0;
    int victim = top[0];
    Player& v = players[victim];
    v.alive = false;
    out += "\n" + v.name + " received " + std::to_string(best) + " vote(s).\n" +
           v.name + " has been eliminated.\nRole revealed: " + v.role + "\n";
    if (v.role != "SPY") out += "The Spy is still among you...";
    info(out);
    srv.send_to(victim, {"INFO", "You are now a spectator. You can watch but not talk or vote."});
    printf("eliminated: %s (%s)\n", v.name.c_str(), v.role.c_str());

    if (check_win()) return;
    start_round();
}

static void start_game() {
    std::vector<std::string> roles = {"SPY", "DETECTIVE", "AGENT", "AGENT", "AGENT"};
    std::mt19937 rng(std::random_device{}());
    std::shuffle(roles.begin(), roles.end(), rng);
    info("5/5 players connected. Starting game...");
    int i = 0;
    for (auto& kv : players) {
        Player& p = kv.second;
        p.role = roles[i++];
        p.alive = true;
        srv.send_to(kv.first, {"ROLE", p.role});                       // unicast
        if (p.role == "SPY")
            srv.send_to(kv.first, {"INFO", "Stay hidden and avoid elimination."});
        else if (p.role == "DETECTIVE")
            srv.send_to(kv.first, {"INFO", "Each round you get to investigate one player."});
        else
            srv.send_to(kv.first, {"INFO", "Work with the others to identify the Spy."});
        printf("assigned: %s -> %s\n", p.name.c_str(), p.role.c_str());
    }
    round_no = 0;
    ties_in_row = 0;
    start_round();
}

// ---------- connection events ----------
static void handle_join(int fd) {
    Peer* pe = srv.peer(fd);
    if (!pe) return;
    bool dup = false;
    for (auto& kv : players) if (kv.second.uid == pe->uid) dup = true;
    if (phase != Phase::LOBBY || (int)players.size() >= NUM_PLAYERS || dup) {
        err(fd, "Cannot join: game full, started, or duplicate user.");
        printf("rejected: fd=%d uid=%d\n", fd, (int)pe->uid);
        srv.kick(fd);
        return;
    }
    std::string name;
    for (int n = 1; n <= NUM_PLAYERS; n++) {
        std::string c = "Player" + std::to_string(n);
        if (find_by_name(c) < 0) { name = c; break; }
    }
    Player p;
    p.name = name;
    p.uid = pe->uid;
    players[fd] = p;
    pe->name = name;
    printf("joined: %s (uid=%d)\n", name.c_str(), (int)pe->uid);
    info(name + " joined. (" + std::to_string(players.size()) + "/" +
         std::to_string(NUM_PLAYERS) + ")");
}

static void handle_leave(int fd) {
    auto it = players.find(fd);
    if (it == players.end()) return;
    Player& p = it->second;
    printf("left: %s\n", p.name.c_str());

    if (phase == Phase::LOBBY) {
        std::string n = p.name;
        players.erase(it);
        info(n + " left. (" + std::to_string(players.size()) + "/" +
             std::to_string(NUM_PLAYERS) + ")");
        return;
    }
    if (!p.alive) return;                         // a spectator leaving
    p.alive = false;
    info(p.name + " disconnected and is out of the game. Role: " + p.role);
    if (check_win()) return;
    if (phase == Phase::INVESTIGATION && !detective_alive()) start_discussion();
    else if (phase == Phase::VOTING && all_voted()) tally_votes();
}

// ---------- player input ----------
static void handle_chat(int fd, const std::string& raw) {
    std::string text = trim(raw);
    if (text.empty()) return;
    Player& me = players[fd];

    if (phase == Phase::LOBBY) {
        srv.broadcast({"CHAT", me.name + ": " + text});
        return;
    }

    if (!me.alive) {
        err(fd, "Eliminated players are spectators and cannot talk or vote.");
        return;
    }

    // ----- investigation: the Detective just types a name -----
    if (phase == Phase::INVESTIGATION) {
        if (me.role != "DETECTIVE") { err(fd, "The Detective is investigating. Please wait."); return; }
        int tfd = parse_target(text);
        if (tfd < 0 || tfd == fd || !players[tfd].alive) {
            err(fd, "Type the name of a living player other than yourself, e.g. Player3.");
            return;
        }
        bool is_spy = players[tfd].role == "SPY";
        srv.send_to(fd, {"RESULT", players[tfd].name + (is_spy ? " IS THE SPY." : " is NOT the Spy.")});
        printf("investigate: %s -> %s (%s)\n", me.name.c_str(),
               players[tfd].name.c_str(), is_spy ? "SPY" : "not spy");
        info("The Detective has finished investigating.");
        start_discussion();                       // do not touch `me` after this
        return;
    }

    // ----- voting: just type a name -----
    if (phase == Phase::VOTING) {
        if (votes.count(fd)) { err(fd, "You already voted. Votes are final."); return; }
        int tfd = parse_target(text);
        if (tfd < 0 || tfd == fd || !players[tfd].alive) {
            err(fd, "Type the name of a living player other than yourself, e.g. Player3.");
            return;
        }
        votes[fd] = tfd;
        srv.send_to(fd, {"INFO", "[vote recorded for " + players[tfd].name + "]"});
        int voted = 0;
        for (auto& kv : players) if (kv.second.alive && votes.count(kv.first)) voted++;
        info(me.name + " has voted. (" + std::to_string(voted) + "/" +
             std::to_string(count_alive()) + ")");
        printf("vote: %s -> %s\n", me.name.c_str(), players[tfd].name.c_str());
        if (all_voted()) tally_votes();           // may end the game: do not touch `me` after this
        return;
    }

    // ----- discussion -----
    std::istringstream in(text);
    std::string cmd;
    in >> cmd;
    if (cmd == "/private") {
        std::string target, body;
        in >> target;
        std::getline(in, body);
        body = trim(body);
        int tfd = find_by_name(target);
        if (tfd < 0 || tfd == fd || !players[tfd].alive || body.empty()) {
            err(fd, "Usage: /private PlayerN message (a living player other than yourself)");
            return;
        }
        srv.send_to(tfd, {"PRIVATE", me.name + "|" + body});          // unicast
        srv.send_to(fd, {"INFO", "[sent to " + players[tfd].name + "]"});
        printf("private: %s -> %s\n", me.name.c_str(), players[tfd].name.c_str());
    } else if (cmd[0] == '/') {
        err(fd, "Unknown command. In discussion you can use /private PlayerN message.");
    } else {
        srv.broadcast({"CHAT", me.name + ": " + text});               // broadcast
    }
}

// ---------- main loop ----------
int main() {
    if (!srv.listen_on("/tmp/spyhunt/spyhunt.sock")) { perror("listen"); return 1; }
    printf("server listening, waiting for %d players\n", NUM_PLAYERS);

    while (true) {
        std::vector<std::pair<int, Message>> msgs;
        std::vector<int> joined, left;
        srv.wait_events(500, msgs, joined, left);

        for (int fd : joined) handle_join(fd);
        for (int fd : left) handle_leave(fd);

        if (phase == Phase::LOBBY && (int)players.size() == NUM_PLAYERS) start_game();

        auto now = Clock::now();
        if (phase == Phase::INVESTIGATION && now >= phase_end) {
            info("The Detective ran out of time.");
            start_discussion();
        } else if (phase == Phase::DISCUSSION && now >= phase_end) {
            start_voting();
        } else if (phase == Phase::VOTING && now >= phase_end) {
            tally_votes();
        }

        for (auto& item : msgs) {
            if (!players.count(item.first)) continue;
            if (item.second.type != "CHAT") continue;
            handle_chat(item.first, item.second.text);
        }
        fflush(stdout);
    }
}
