// communication.h - Spy Hunt communications library (single file)
#pragma once
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace spy {

constexpr uint32_t MAX_MSG = 4096;

struct Message {
    std::string type;
    std::string text;
};

inline bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n;
        len -= n;
    }
    return true;
}

inline bool read_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        ssize_t n = ::read(fd, p, len);
        if (n == 0) return false;
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n;
        len -= n;
    }
    return true;
}

inline bool send_msg(int fd, const Message& m) {
    std::string payload = m.type + "|" + m.text;
    if (payload.size() > MAX_MSG) return false;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    return write_all(fd, &len, 4) && write_all(fd, payload.data(), payload.size());
}

inline bool recv_msg(int fd, Message& m) {
    uint32_t len;
    if (!read_all(fd, &len, 4)) return false;
    len = ntohl(len);
    if (len == 0 || len > MAX_MSG) return false;
    std::string payload(len, '\0');
    if (!read_all(fd, &payload[0], len)) return false;
    size_t bar = payload.find('|');
    if (bar == std::string::npos) {
        m.type = payload;
        m.text.clear();
    } else {
        m.type = payload.substr(0, bar);
        m.text = payload.substr(bar + 1);
    }
    return true;
}

// ---------- Server side ----------
struct Peer {
    int fd = -1;
    uid_t uid = 0;
    pid_t pid = 0;
    std::string inbuf;
    std::string name;
};

class Server {
public:
    std::map<int, Peer> peers;

    bool listen_on(const char* path, mode_t mode = 0660) {
        ::unlink(path);
        lfd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0) return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        if (::bind(lfd, (sockaddr*)&a, sizeof a) < 0) return false;
        ::chmod(path, mode);
        if (::listen(lfd, 8) < 0) return false;
        ep = ::epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = lfd;
        ::epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
        return true;
    }

    void wait_events(int timeout_ms,
                     std::vector<std::pair<int, Message>>& msgs,
                     std::vector<int>& joined,
                     std::vector<int>& left) {
        epoll_event evs[16];
        int n = ::epoll_wait(ep, evs, 16, timeout_ms);
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == lfd) { accept_one(joined); continue; }
            if (!peers.count(fd)) continue;
            if (!read_peer(fd, msgs)) { drop(fd); left.push_back(fd); }
        }
    }

    bool send_to(int fd, const Message& m) {
        if (!peers.count(fd)) return false;
        return send_msg(fd, m);
    }

    void multicast(const std::vector<int>& fds, const Message& m) {
        for (int fd : fds) send_to(fd, m);
    }

    void broadcast(const Message& m) {
        for (auto& kv : peers) send_to(kv.first, m);
    }

    Peer* peer(int fd) {
        auto it = peers.find(fd);
        return it == peers.end() ? nullptr : &it->second;
    }

    void kick(int fd) {
        if (peers.count(fd)) drop(fd);
    }

private:
    int lfd = -1, ep = -1;

    void accept_one(std::vector<int>& joined) {
        int c = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
        if (c < 0) return;
        ucred cr{};
        socklen_t len = sizeof cr;
        ::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &len);
        Peer p;
        p.fd = c; p.uid = cr.uid; p.pid = cr.pid;
        peers[c] = p;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = c;
        ::epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
        joined.push_back(c);
    }

    bool read_peer(int fd, std::vector<std::pair<int, Message>>& msgs) {
        Peer& p = peers[fd];
        char buf[1024];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n == 0) return false;
        if (n < 0) return (errno == EAGAIN || errno == EINTR);
        p.inbuf.append(buf, n);
        while (p.inbuf.size() >= 4) {
            uint32_t len;
            memcpy(&len, p.inbuf.data(), 4);
            len = ntohl(len);
            if (len == 0 || len > MAX_MSG) return false;
            if (p.inbuf.size() < 4 + len) break;
            std::string payload = p.inbuf.substr(4, len);
            p.inbuf.erase(0, 4 + len);
            Message m;
            size_t bar = payload.find('|');
            if (bar == std::string::npos) { m.type = payload; }
            else { m.type = payload.substr(0, bar); m.text = payload.substr(bar + 1); }
            msgs.push_back({fd, m});
        }
        return true;
    }

    void drop(int fd) {
        ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        peers.erase(fd);
    }
};

// ---------- Client side ----------
class Client {
public:
    enum Event { NONE = 0, KEYBOARD = 1, SERVER = 2 };

    ~Client() { close_conn(); }

    // Connect to the server's UNIX socket.
    bool connect_to(const char* path) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        if (::connect(fd, (sockaddr*)&a, sizeof a) < 0) {
            int e = errno;
            ::close(fd);
            fd = -1;
            errno = e;
            return false;
        }
        return true;
    }

    // Send one message to the server.
    bool send(const Message& m) { return send_msg(fd, m); }

    // Receive one message from the server. False means the server closed.
    bool recv(Message& m) { return recv_msg(fd, m); }

    // Wait until the keyboard (stdin) or the server socket is ready.
    // Returns a bitmask of KEYBOARD and/or SERVER (NONE on timeout).
    // This is what lets server messages arrive while the player is idle.
    int wait(int timeout_ms = -1) {
        pollfd fds[2] = {{0, POLLIN, 0}, {fd, POLLIN, 0}};
        int r = ::poll(fds, 2, timeout_ms);
        if (r <= 0) return NONE;
        int ev = NONE;
        if (fds[0].revents & (POLLIN | POLLHUP)) ev |= KEYBOARD;
        if (fds[1].revents & (POLLIN | POLLHUP)) ev |= SERVER;
        return ev;
    }

    void close_conn() {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }

private:
    int fd = -1;
};

} // namespace spy
