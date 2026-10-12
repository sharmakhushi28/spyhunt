#include "communication.h"
#include <poll.h>
#include <iostream>

int main() {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/tmp/spyhunt/spyhunt.sock", sizeof(a.sun_path) - 1);
    if (connect(s, (sockaddr*)&a, sizeof a) < 0) { perror("connect"); return 1; }
    printf("connected. type messages, Ctrl+D to quit\n");

    pollfd fds[2] = {{0, POLLIN, 0}, {s, POLLIN, 0}};
    while (true) {
        poll(fds, 2, -1);
        if (fds[0].revents & POLLIN) {
            std::string line;
            if (!std::getline(std::cin, line)) break;
            spy::send_msg(s, {"CHAT", line});
        }
        if (fds[1].revents & POLLIN) {
            spy::Message m;
            if (!spy::recv_msg(s, m)) { printf("server closed\n"); break; }
            printf("[%s] %s\n", m.type.c_str(), m.text.c_str());
        }
    }
}
