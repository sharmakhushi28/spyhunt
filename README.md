# Spy Hunt

A text-based social deduction game for **5 players**, inspired by games such as Werewolves and Mafia. Spy Hunt is written in **C++17** and designed to demonstrate Linux interprocess communication (IPC), concurrency, UNIX processes, and communication between separate Linux users.

Each player runs as a **separate process under a separate Linux user account**. All game communication is handled through a single-file communications library using **UNIX domain stream sockets**. The server uses **epoll** to handle multiple player connections concurrently.

**Author:** Khushi Sharma

---

## Game Overview

Spy Hunt is played by five players with randomly assigned secret roles:

- **1 Spy**
- **1 Detective**
- **3 Agents**

The Agents and Detective work together to identify and eliminate the Spy.

The Spy tries to remain hidden and survive the voting rounds.

The game contains:

- Secret role assignment
- Detective investigation
- Public discussion
- Private player-to-player messages
- Voting
- Player elimination
- Spectator mode
- Multiple rounds
- Win conditions
- Automatic game reset

---

# Communications Library

> **`communication.h`** is the single-file communications library used by Spy Hunt.

It is located in the project root:

```text
~/spyhunt/communication.h
```

The library is header-only, and the communication functionality is contained in this single file.

The communication code is organized inside:

```cpp
namespace spy
```

The library handles:

- UNIX domain stream sockets
- message framing
- sending and receiving messages
- multiple simultaneous clients
- non-blocking server-side sockets
- `epoll` event handling
- client UID/PID identification
- unicast
- multicast
- broadcast
- connection handling
- disconnection handling

---

# Communication Primitive

Spy Hunt uses:

```text
UNIX Domain Stream Sockets
AF_UNIX + SOCK_STREAM
```

The server creates the socket:

```text
/tmp/spyhunt/spyhunt.sock
```

All player processes connect to this socket.

The basic architecture is:

```text
Player1 ──┐
Player2 ──┤
Player3 ──┼── UNIX Domain Stream Sockets ── Server
Player4 ──┤                                  |
Player5 ──┘                                epoll
```

UNIX domain sockets allow separate processes on the same Linux system to communicate without using network IP addresses or TCP ports.

---

# Message Framing

`SOCK_STREAM` provides a continuous stream of bytes and does not preserve individual message boundaries.

Therefore, Spy Hunt implements **length-prefixed message framing**.

Each message is transmitted as:

```text
[4-byte message length][TYPE|text]
```

The length is stored in network byte order.

Example logical messages include:

```text
CHAT|Hello everyone
ROLE|SPY
PRIVATE|Player2|I think Player4 is suspicious
RESULT|Player4 IS THE SPY
INFO|Voting has started
ERROR|You cannot vote for yourself
```

The maximum message size is:

```text
4096 bytes
```

A message containing an invalid length, including a zero length or a length larger than the configured maximum, is rejected.

---

# Communications Library API

## `Message`

Represents one logical message.

```cpp
struct Message {
    std::string type;
    std::string text;
};
```

Common message types include:

```text
CHAT
ROLE
PRIVATE
RESULT
INFO
ERROR
```

---

## `Peer`

Represents a connected client from the server's perspective.

It stores information including:

```text
fd
uid
pid
inbuf
name
```

The UID and PID are obtained from the Linux kernel using `SO_PEERCRED`.

---

## Message Functions

| Function | Description |
|---|---|
| `write_all(fd, buf, len)` | Writes exactly the requested number of bytes and handles partial writes and `EINTR`. Uses `MSG_NOSIGNAL`. |
| `read_all(fd, buf, len)` | Reads exactly the requested number of bytes. Returns false on EOF or error. |
| `send_msg(fd, Message)` | Frames and sends one complete message. |
| `recv_msg(fd, Message)` | Receives and parses one complete framed message. |

---

## `spy::Server`

The server-side communication interface is provided by the `Server` class in `communication.h`.

| Method | Description |
|---|---|
| `listen_on(path, mode)` | Creates and binds the UNIX socket, begins listening, and creates the epoll instance. |
| `wait_events(timeout_ms, msgs, joined, left)` | Waits for socket activity using `epoll_wait()`, accepts connections, receives available data, returns complete messages, and reports disconnects. |
| `send_to(fd, msg)` | Sends a message to one specific client (**unicast**). |
| `multicast(fds, msg)` | Sends a message to a selected group of clients (**multicast**). |
| `broadcast(msg)` | Sends a message to every connected client (**broadcast**). |
| `peer(fd)` | Returns information about a connected peer. |
| `kick(fd)` | Disconnects a specific client. |
| `peers` | Stores the currently connected clients. |

---

# How the Communication System Works

The server uses **epoll** to monitor:

- the listening socket
- Player1's socket
- Player2's socket
- Player3's socket
- Player4's socket
- Player5's socket

Conceptually:

```text
                 SERVER
                    |
                  epoll
                    |
        +-----------+-----------+
        |      |       |       |
       P1     P2      P3      P4      P5
```

The server does not need to block while waiting for one particular player.

When Linux reports that a player's socket has data available, the server processes that player's data and continues handling the other connections.

---

## Per-Client Input Buffers

Each connected player has an input buffer:

```cpp
Peer::inbuf
```

Bytes received from the socket are added to this buffer.

The server processes a message only when the complete length-prefixed message has arrived.

Conceptually:

```text
Incoming bytes
      |
      v
Peer::inbuf
      |
      v
Complete message?
   /       \
  No       Yes
  |         |
wait      process
```

This prevents a partial message from blocking the entire game server.

---

# Linux User Identification

Each player runs under a separate Linux user account.

Example:

```text
player1
player2
player3
player4
player5
```

When a player connects, the server uses:

```cpp
getsockopt(..., SO_PEERCRED, ...)
```

to obtain the connecting process's:

- Linux UID
- PID

These credentials are supplied by the Linux kernel rather than by the client.

This means the client cannot simply send a message claiming to be another Linux user.

The server also rejects a second connection from the same Linux UID so that one Linux user cannot occupy multiple player positions.

---

# Communication Patterns

Spy Hunt demonstrates several IPC communication patterns.

| Game Event | Communication Pattern |
|---|---|
| Public discussion | Player → Server → **Broadcast** |
| `/private PlayerN message` | Player → Server → **Unicast** |
| Secret role assignment | Server → Player **Unicast** |
| Detective investigation result | Server → Detective **Unicast** |
| Player votes | Individual messages to Server |
| Voting results | Server → **Broadcast** |
| Phase changes | **Broadcast** |
| Player elimination | **Broadcast** |
| Game-over information | **Broadcast** |

The communications library also provides:

```cpp
multicast()
```

which allows the server to send a message to a selected group of connected clients.

---

# Client-Side Concurrency

`player.cpp` uses:

```text
poll()
```

to monitor both:

- keyboard input (`stdin`)
- the server socket

This means a player can receive server messages while the program is waiting for keyboard input.

---

# Project Files

| File | Purpose |
|---|---|
| `communication.h` | **Single-file communications library.** |
| `server.cpp` | Server-side game logic and phase state machine. |
| `player.cpp` | Terminal client run by each player. |
| `test_frame.cpp` | Test program for length-prefixed message framing using `socketpair()`. |
| `Makefile` | Builds the server, player, and framing test. |
| `README.md` | Setup, compilation, gameplay, API, and project documentation. |

---

# Setup

The following instructions assume Ubuntu/Linux.

## 1. Install Required Packages

```bash
sudo apt update
sudo apt install build-essential util-linux-extra acl
```

---

## 2. Create the Spy Hunt Group

```bash
sudo groupadd spyhunt
```

If the group already exists, this command can be skipped.

---

## 3. Create Five Linux Player Accounts

Each player must run under a different Linux user account.

```bash
for i in 1 2 3 4 5; do
    sudo useradd -m -s /bin/bash -G spyhunt player$i
done
```

Set passwords for the player accounts:

```bash
sudo passwd player1
sudo passwd player2
sudo passwd player3
sudo passwd player4
sudo passwd player5
```

Passwords should not be stored in the source code or Git repository.

---

## 4. Add the Project Owner to the Group

```bash
sudo usermod -aG spyhunt $USER
```

Log out and log back in if necessary for the group membership to become active.

Verify:

```bash
id
```

The output should include:

```text
spyhunt
```

---

## 5. Allow the Spy Hunt Group to Access the Project

Assuming the repository is stored at:

```text
~/spyhunt
```

run:

```bash
sudo chgrp -R spyhunt ~/spyhunt
chmod -R g+rX ~/spyhunt
chmod g+s ~/spyhunt
```

If the home directory itself prevents other users from traversing it, give the `spyhunt` group traverse-only access:

```bash
setfacl -m g:spyhunt:x "$HOME"
```

This allows the player accounts to reach the project without giving them general read access to the rest of the home directory.

---

# Runtime Socket Directory

The game uses:

```text
/tmp/spyhunt
```

Because `/tmp` may be cleared after a reboot, recreate the directory when necessary:

```bash
sudo mkdir -p /tmp/spyhunt
sudo chgrp spyhunt /tmp/spyhunt
sudo chmod 2770 /tmp/spyhunt
```

Verify:

```bash
ls -ld /tmp/spyhunt
```

The result should show the group:

```text
spyhunt
```

and permissions similar to:

```text
drwxrws---
```

---

# Build

Go to the project directory:

```bash
cd ~/spyhunt
```

Build the server and player using the provided Makefile:

```bash
make
```

This creates:

```text
server
player
```

Give the `spyhunt` group permission to execute them:

```bash
chmod g+rx server player
```

---

## Build the Message-Framing Test

```bash
make test_frame
```

Run it:

```bash
./test_frame
```

Expected output:

```text
type=CHAT text=hello everyone
type=VOTE text=Player2
```

---

## Clean Compiled Files

```bash
make clean
```

This removes:

```text
server
player
test_frame
*.o
```

After cleaning, rebuild the game with:

```bash
make
chmod g+rx server player
```

---

## Manual Compilation

The programs can also be compiled manually:

```bash
g++ -std=c++17 -Wall -Wextra -o server server.cpp
g++ -std=c++17 -Wall -Wextra -o player player.cpp
chmod g+rx server player
```

---

# Running the Game

Before starting the game, make sure the runtime socket directory exists:

```bash
ls -ld /tmp/spyhunt
```

If it does not exist:

```bash
sudo mkdir -p /tmp/spyhunt
sudo chgrp spyhunt /tmp/spyhunt
sudo chmod 2770 /tmp/spyhunt
```

---

## 1. Start the Server

From the project directory:

```bash
cd ~/spyhunt
./server
```

The server should display:

```text
server listening, waiting for 5 players
```

Leave this terminal running.

---

## 2. Start the Five Players

Open five additional terminals.

Player 1:

```bash
su - player1 -c /home/<your-user>/spyhunt/player
```

Player 2:

```bash
su - player2 -c /home/<your-user>/spyhunt/player
```

Player 3:

```bash
su - player3 -c /home/<your-user>/spyhunt/player
```

Player 4:

```bash
su - player4 -c /home/<your-user>/spyhunt/player
```

Player 5:

```bash
su - player5 -c /home/<your-user>/spyhunt/player
```

Replace:

```text
<your-user>
```

with the Linux username that owns the project.

Player names are assigned according to **connection order**.

The first accepted connection becomes:

```text
Player1
```

the second becomes:

```text
Player2
```

and so on.

When the fifth player connects, the game starts automatically.

---

# Game Rules

Spy Hunt has five players.

Roles are randomly assigned:

```text
1 Spy
1 Detective
3 Agents
```

Each player receives their role privately.

---

# Round Structure

Every round follows:

```text
INVESTIGATION
      |
      v
DISCUSSION
      |
      v
VOTING
      |
      v
RESULTS
```

---

# 1. Investigation Phase

The Investigation phase lasts up to **60 seconds**.

Only the Detective can make an investigation.

The Detective types the name of a living player.

Example:

```text
Player4
```

The Detective privately receives:

```text
[INVESTIGATION RESULT]
Player4 IS THE SPY.
```

or:

```text
[INVESTIGATION RESULT]
Player4 is NOT the Spy.
```

Other players do not receive this result.

The Investigation phase ends early after the Detective makes a valid choice.

If the Detective does not choose within the time limit, the game continues to Discussion.

---

# 2. Discussion Phase

The Discussion phase lasts **180 seconds**.

During this phase, normal text is public.

Example:

```text
I think Player4 is suspicious
```

Everyone receives:

```text
Player1: I think Player4 is suspicious
```

Players can discuss behavior, investigation information, previous votes, or attempt to mislead other players.

---

## Private Messages

During Discussion, a player can send a private message using:

```text
/private Player3 I think Player4 is the Spy
```

Only Player3 receives:

```text
[PRIVATE - Player1] I think Player4 is the Spy
```

The other players do not receive the private message.

---

# 3. Voting Phase

The Voting phase lasts up to **120 seconds**.

Players do not need to use a `/vote` command.

Each living player simply types the name of the player they want to eliminate.

Example:

```text
Player4
```

The server records the vote.

A player:

- can vote only once
- cannot vote for themselves
- cannot vote for an eliminated player

The server announces voting progress without immediately revealing the selected target:

```text
Player1 has voted. (1/5)
Player2 has voted. (2/5)
```

Voting ends early when every living player has voted.

---

# Voting Results

After voting finishes, the server broadcasts the full results.

Example:

```text
========== RESULTS ==========

Player1 -> Player4
Player2 -> Player4
Player3 -> Player2
Player4 -> Player2
Player5 -> Player4
```

The player receiving the most votes is eliminated.

Their role is revealed:

```text
Player4 has been eliminated.
Role revealed: AGENT
```

If the eliminated player is not the Spy, another round begins.

---

# Spectator Mode

Eliminated players remain connected as spectators.

A spectator:

- can continue watching the game
- receives public game information
- cannot participate in discussion
- cannot send private messages
- cannot investigate
- cannot vote

If a spectator tries to participate, the server rejects the action.

Example:

```text
[ERROR] Eliminated players are spectators and cannot talk or vote.
```

---

# Winning Conditions

## Agents and Detective Win

The Agents and Detective win when the Spy is eliminated.

Example:

```text
========== GAME OVER ==========

THE SPY HAS BEEN ELIMINATED. AGENTS WIN!
```

At game over, all roles are revealed.

---

## Spy Wins

The Spy wins if the Spy survives until only **two living players remain**.

Example:

```text
Only 2 players remain. THE SPY WINS!
```

A tied vote or no votes causes no elimination.

If voting fails because of a tie or no-vote condition **two times in a row**, the Spy wins. This prevents the game from continuing indefinitely.

A player who disconnects during an active game is treated as eliminated.

---

# Automatic Game Reset

After GAME OVER:

1. The server reveals all roles.
2. Connected players are disconnected.
3. The current game state is cleared.
4. The server returns to the lobby.
5. Five new players can connect and start another game.

The server process does not need to be restarted manually.

---

# Security and Robustness

Spy Hunt includes several security and robustness features.

## UNIX Socket Permissions

The socket uses mode:

```text
0660
```

and is stored inside:

```text
/tmp/spyhunt
```

which uses:

```text
group: spyhunt
mode: 2770
```

This restricts access to authorized users.

---

## Kernel-Supplied Identity

The server obtains client UID and PID values using:

```text
SO_PEERCRED
```

These values are supplied by the Linux kernel rather than by client-provided messages.

A client therefore cannot simply claim another Linux user's UID.

The server rejects a second connection from the same UID.

---

## Message Framing

Messages use a four-byte length prefix.

This prevents the program from assuming:

```text
one write() = one read()
```

which is not guaranteed by stream sockets.

---

## Maximum Message Size

Messages are limited to:

```text
4096 bytes
```

Invalid or oversized message lengths are rejected.

---

## Non-Blocking Server Input

Connected client sockets are non-blocking.

The server uses:

```text
epoll
```

and maintains a separate input buffer for each player.

This allows multiple players to communicate concurrently without one partially received message blocking the other players.

---

## Disconnect Handling

A player disconnecting does not crash the server.

If a living player disconnects during a game, that player is treated as eliminated.

---

## SIGPIPE Protection

Outgoing messages use:

```text
MSG_NOSIGNAL
```

which prevents the server from being terminated by `SIGPIPE` if a client disconnects while data is being sent.

---

## Server-Side Validation

The server validates:

- current game phase
- player role
- whether a player is alive
- investigation targets
- private-message targets
- voting targets
- duplicate Linux users

The game does not depend on the client to enforce these rules.

---

# Known Limitations

- Incoming server messages may appear while a player is typing, which can make terminal output visually messy.
- Chat text is not currently filtered for terminal control characters.
- Server-side outgoing messages use `write_all()` instead of an asynchronous per-client output queue. Therefore, a sufficiently slow or stalled receiver could delay the server while a message is being sent.
- `player.cpp` creates its client socket directly rather than using a dedicated client class inside `communication.h`.
- Player names are assigned according to connection order rather than being selected by the players.

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
                    |
                    v
               INVESTIGATION
                    |
                    v
                DISCUSSION
                    |
                    v
                  VOTING
```

---

# Author

**Khushi Sharma**

Spy Hunt was developed as a C++ Linux IPC and concurrency project using UNIX domain stream sockets, `epoll`, message framing, separate Linux users/processes, and multiple communication patterns.
