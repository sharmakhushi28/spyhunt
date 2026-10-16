# Spy Hunt

A text-based social deduction game for **5 players**, inspired by Werewolves and Mafia. Spy Hunt is written in **C++17** and demonstrates Linux interprocess communication (IPC), concurrency, UNIX processes, and communication between separate Linux users.

Each player runs as a **separate process under a separate Linux user account**. All game communication goes through a single-file communications library using **UNIX domain stream sockets**. The server uses **epoll** to handle multiple player connections concurrently.

**Author:** Khushi Sharma

---

## Game Overview

Five players receive randomly assigned secret roles:

- **1 Spy**
- **1 Detective**
- **3 Agents**

The Agents and Detective work together to eliminate the Spy. The Spy tries to stay hidden and survive the voting rounds.

The game includes secret role assignment, Detective investigation, public discussion, private player-to-player messages, voting, elimination, spectator mode, multiple rounds, win conditions, and automatic game reset.

---

# Communications Library

> **`communication.h`** is the single-file communications library used by Spy Hunt.

It is located in the project root:

```text
~/spyhunt/communication.h
```

The library is header-only, so all communication code lives in this one file, inside `namespace spy`. Both the server and the player client use it. Neither `server.cpp` nor `player.cpp` calls `socket`, `connect`, `epoll`, or `poll` directly.

The library handles:

- UNIX domain stream sockets (server and client side)
- message framing
- sending and receiving messages
- multiple simultaneous clients
- non-blocking server-side sockets
- `epoll` event handling
- client UID/PID identification
- unicast, multicast, and broadcast
- connection and disconnection handling
- client-side waiting on keyboard and socket together

---

# Communication Primitive

Spy Hunt uses **UNIX domain stream sockets** (`AF_UNIX` + `SOCK_STREAM`).

The server creates the socket `/tmp/spyhunt/spyhunt.sock`, and all player processes connect to it.

```text
Player1 ──┐
Player2 ──┤
Player3 ──┼── UNIX Domain Stream Sockets ── Server
Player4 ──┤                                  |
Player5 ──┘                                epoll
```

UNIX domain sockets let separate processes on the same machine communicate without IP addresses or TCP ports.

---

# Message Framing

`SOCK_STREAM` is a continuous stream of bytes and does not preserve message boundaries. Spy Hunt therefore uses **length-prefixed framing**. Each message is transmitted as:

```text
[4-byte length, network byte order]["TYPE|text"]
```

Example logical messages:

```text
CHAT|Hello everyone
ROLE|SPY
PRIVATE|Player2|I think Player4 is suspicious
RESULT|Player4 IS THE SPY
INFO|Voting has started
ERROR|You cannot vote for yourself
```

`MAX_MSG` is **4096 bytes**. A message with a zero length or a length above `MAX_MSG` is rejected and the sender is dropped.

---

# Communications Library API

## Data types

| Name | Description |
|---|---|
| `Message { std::string type, text; }` | One logical message. Types include `CHAT`, `ROLE`, `PRIVATE`, `RESULT`, `INFO`, `ERROR`. |
| `Peer { fd, uid, pid, inbuf, name }` | A connected client as seen by the server. UID and PID come from the kernel via `SO_PEERCRED`. |

## Message functions

| Function | Description |
|---|---|
| `bool write_all(fd, buf, len)` | Writes exactly `len` bytes, handling partial writes and `EINTR`. Uses `MSG_NOSIGNAL`. |
| `bool read_all(fd, buf, len)` | Reads exactly `len` bytes. Returns false on EOF or error. |
| `bool send_msg(fd, const Message&)` | Frames and sends one message. |
| `bool recv_msg(fd, Message&)` | Receives and parses one complete framed message. |

## `spy::Server` (used by `server.cpp`)

| Method | Description |
|---|---|
| `bool listen_on(path, mode = 0660)` | Creates the UNIX socket, sets its permissions, starts listening, and creates the epoll instance. |
| `void wait_events(timeout_ms, msgs, joined, left)` | One `epoll_wait` pass. Accepts new clients (into `joined`), reads available data, returns each complete message in `msgs` as `(fd, Message)`, and reports disconnects in `left`. |
| `bool send_to(fd, msg)` | Sends to one client (**unicast**). |
| `void multicast(fds, msg)` | Sends to a chosen list of clients (**multicast**). |
| `void broadcast(msg)` | Sends to every connected client (**broadcast**). |
| `Peer* peer(fd)` | Looks up a connected client (UID, PID, name). |
| `void kick(fd)` | Disconnects a client. |
| `std::map<int, Peer> peers` | All currently connected clients. |

## `spy::Client` (used by `player.cpp`)

| Method | Description |
|---|---|
| `bool connect_to(path)` | Connects to the server's UNIX socket. |
| `bool send(msg)` | Sends one framed message to the server. |
| `bool recv(msg)` | Receives one framed message. Returns false if the server closed the connection. |
| `int wait(timeout_ms = -1)` | Calls `poll()` on stdin and the server socket together. Returns a bitmask of `Client::KEYBOARD` and/or `Client::SERVER` (`Client::NONE` on timeout). |
| `void close_conn()` | Closes the connection. |

---

# How the Communication System Works

## Server: epoll

The server uses **epoll** in a single thread to watch the listening socket and every player socket. It never blocks on one particular player. When Linux reports that a socket has data, the server handles it and moves on.

```text
                 SERVER
                    |
                  epoll
                    |
        +-----------+-----------+
        |      |       |       |
       P1     P2      P3      P4      P5
```

## Per-client input buffers

Each connected player has an input buffer (`Peer::inbuf`). Received bytes are appended to it, and a message is processed only once the complete length-prefixed message has arrived.

```text
Incoming bytes -> Peer::inbuf -> complete message?
                                   /        \
                                  no        yes
                                  |          |
                                wait       process
```

A client that sends only half a message cannot block the server or other players.

## Client: poll

`player.cpp` calls `Client::wait()`, which uses `poll()` on both the keyboard (`stdin`) and the server socket. A player therefore receives server messages even while idle at the keyboard.

---

# Linux User Identification

Each player runs under a separate Linux account (`player1` to `player5`).

When a player connects, the server calls `getsockopt(..., SO_PEERCRED, ...)` to get the connecting process's Linux UID and PID. The kernel supplies these values, not the client, so a client cannot claim to be another user.

The server rejects a second connection from the same UID, so one Linux user cannot hold two seats.

---

# Communication Patterns

| Game Event | Pattern |
|---|---|
| Public discussion | Player → Server → **Broadcast** |
| `/private PlayerN message` | Player → Server → **Unicast** |
| Secret role assignment | Server → Player **Unicast** |
| Detective investigation result | Server → Detective **Unicast** |
| Player votes | Individual messages to Server |
| Voting results | Server → **Broadcast** |
| Phase changes, eliminations, game over | **Broadcast** |

`Server::multicast()` is also provided for sending to a selected group of clients.

---

# Project Files

| File | Purpose |
|---|---|
| `communication.h` | **Single-file communications library (server and client).** |
| `server.cpp` | Server-side game logic and phase state machine. |
| `player.cpp` | Terminal client run by each player, built on `spy::Client`. |
| `test_frame.cpp` | Test of the length-prefixed framing using `socketpair()`. |
| `Makefile` | Builds the server, player, and framing test. |
| `README.md` | Setup, build, gameplay, API, and documentation. |

---

# Setup

These instructions assume Ubuntu/Linux.

## 1. Install required packages

```bash
sudo apt update
sudo apt install build-essential util-linux-extra acl
```

## 2. Create the Spy Hunt group

```bash
sudo groupadd spyhunt
```

Skip this if the group already exists.

## 3. Create five Linux player accounts

```bash
for i in 1 2 3 4 5; do
    sudo useradd -m -s /bin/bash -G spyhunt player$i
done
```

Set a password for each account:

```bash
sudo passwd player1
sudo passwd player2
sudo passwd player3
sudo passwd player4
sudo passwd player5
```

Passwords should not be stored in the source code or the Git repository.

## 4. Add the project owner to the group

```bash
sudo usermod -aG spyhunt $USER
```

Log out and back in for the membership to take effect, then verify with `id`. The output should include `spyhunt`.

## 5. Allow the group to access the project

Assuming the project is at `~/spyhunt`:

```bash
sudo chgrp -R spyhunt ~/spyhunt
chmod -R g+rX ~/spyhunt
chmod g+s ~/spyhunt
```

If your home directory blocks other users, give the group traverse-only access:

```bash
setfacl -m g:spyhunt:x "$HOME"
```

This lets the player accounts reach the project without letting them read the rest of the home directory.

---

# Runtime Socket Directory

The game uses `/tmp/spyhunt`. Because `/tmp` is cleared on reboot, recreate it when necessary:

```bash
sudo mkdir -p /tmp/spyhunt
sudo chgrp spyhunt /tmp/spyhunt
sudo chmod 2770 /tmp/spyhunt
ls -ld /tmp/spyhunt
```

The result should show group `spyhunt` and permissions like `drwxrws---`.

---

# Build

```bash
cd ~/spyhunt
make
chmod g+rx server player
```

This builds `server` and `player`. Warnings from `-Wall -Wextra` are normal.

Build and run the framing test:

```bash
make test_frame
./test_frame
```

Expected output:

```text
type=CHAT text=hello everyone
type=VOTE text=Player2
```

Remove compiled files:

```bash
make clean
```

Manual compilation also works:

```bash
g++ -std=c++17 -Wall -Wextra -o server server.cpp
g++ -std=c++17 -Wall -Wextra -o player player.cpp
chmod g+rx server player
```

---

# Running the Game

Make sure `/tmp/spyhunt` exists (see above).

## 1. Start the server

```bash
cd ~/spyhunt
./server
```

If you have not logged out and back in since joining the `spyhunt` group, use:

```bash
sg spyhunt -c ./server
```

The server prints `server listening, waiting for 5 players`. Leave this terminal running.

## 2. Start the five players

Open five more terminals, one per player:

```bash
su - player1 -c /home/<your-user>/spyhunt/player
su - player2 -c /home/<your-user>/spyhunt/player
su - player3 -c /home/<your-user>/spyhunt/player
su - player4 -c /home/<your-user>/spyhunt/player
su - player5 -c /home/<your-user>/spyhunt/player
```

Replace `<your-user>` with the Linux username that owns the project.

Players are named by **connection order**: the first accepted connection is `Player1`, the second is `Player2`, and so on. The game starts automatically when the fifth player connects.

---

# Game Rules

Roles are randomly assigned and sent privately: 1 Spy, 1 Detective, 3 Agents.

Every round has three phases:

```text
INVESTIGATION -> DISCUSSION -> VOTING -> RESULTS
```

## 1. Investigation (up to 60 seconds)

Only the Detective acts. They type the name of a living player, for example `Player4`, and privately receive:

```text
[INVESTIGATION RESULT]
Player4 IS THE SPY.
```

or:

```text
[INVESTIGATION RESULT]
Player4 is NOT the Spy.
```

Other players do not see the result. The phase ends early once the Detective picks a valid player. If the time runs out, the game moves on to Discussion.

## 2. Discussion (180 seconds)

Normal text is public:

```text
Player1: I think Player4 is suspicious
```

A player can send a private note to one living player:

```text
/private Player3 I think Player4 is the Spy
```

Only Player3 receives `[PRIVATE - Player1] I think Player4 is the Spy`.

## 3. Voting (up to 120 seconds)

No `/vote` command is needed. Each living player simply types a name, for example `Player4`. A player can vote once, cannot vote for themselves, and cannot vote for an eliminated player.

The server announces progress without revealing choices:

```text
Player1 has voted. (1/5)
Player2 has voted. (2/5)
```

Voting ends early when every living player has voted. The server then broadcasts the results:

```text
========== RESULTS ==========
Player1 -> Player4
Player2 -> Player4
Player3 -> Player2
Player4 -> Player2
Player5 -> Player4
```

The player with the most votes is eliminated and their role is revealed.

## Spectator mode

Eliminated players stay connected as spectators. They receive public game information but cannot talk, send private messages, or vote. Trying to do so gives:

```text
[ERROR] Eliminated players are spectators and cannot talk or vote.
```

## Winning

- **Agents and Detective win** when the Spy is eliminated.
- **The Spy wins** when only 2 living players remain.
- A tied vote (or no votes) eliminates nobody. **Two failed votes in a row** is a Spy win, so the game cannot stall forever.
- A player who disconnects during a game is treated as eliminated.

## Automatic reset

After GAME OVER the server reveals all roles, disconnects the players, clears the game state, and returns to the lobby. Five new players can connect without restarting the server.

---

# Security and Robustness

- **Socket permissions:** the socket is mode `0660` inside `/tmp/spyhunt` (group `spyhunt`, mode `2770`), so users outside the group get `Permission denied`.
- **Kernel-supplied identity:** UID and PID come from `SO_PEERCRED`, not from client messages. A second connection from the same UID is rejected.
- **Message framing:** a 4-byte length prefix means the code never assumes one `read()` equals one message.
- **Size limit:** messages are limited to 4096 bytes, and invalid lengths are rejected.
- **Non-blocking server input:** `epoll` plus a separate input buffer per player means partial messages do not block other players.
- **Disconnect handling:** a disconnecting player does not crash the server.
- **SIGPIPE protection:** `MSG_NOSIGNAL` stops the server from being killed if a client disconnects during a send.
- **Server-side validation:** the server checks the phase, the player's role, whether they are alive, and every investigation, private-message, and voting target. It does not rely on the client to enforce rules.

---

# Known Limitations

- Incoming server messages may appear while a player is typing, which can make the terminal output look messy.
- Chat text is not filtered for terminal control characters, so a player could send escape sequences that affect other players' terminals.
- Outgoing server messages use `write_all()` instead of an asynchronous per-client output queue, so a sufficiently slow or stalled receiver could delay the server during a send.
- Player names are assigned by connection order rather than chosen by players.

---

# Example Game Flow

```text
5 PLAYERS CONNECT
        |
        v
SECRET ROLES ASSIGNED
        |
        v
+-------------------------+
| INVESTIGATION (60 sec)  |
| Detective chooses       |
| one player              |
+------------+------------+
             |
             v
+-------------------------+
| DISCUSSION (180 sec)    |
| Public chat             |
| Private messages        |
+------------+------------+
             |
             v
+-------------------------+
| VOTING (120 sec)        |
| Type PlayerN            |
+------------+------------+
             |
             v
        VOTE RESULTS
             |
             v
        ELIMINATION
           /     \
          /       \
       SPY       NOT SPY
        |           |
        v           v
  AGENTS WIN     NEXT ROUND
```

---

# Author

**Khushi Sharma**

Spy Hunt was developed as a C++ Linux IPC and concurrency project using UNIX domain stream sockets, `epoll`, message framing, separate Linux users and processes, and multiple communication patterns.
