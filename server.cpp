#include "communication.h"
#include <cstdio>

int main() {
    spy::Server srv;
    if (!srv.listen_on("/tmp/spyhunt/spyhunt.sock")) { perror("listen"); return 1; }
    printf("server listening\n");

    while (true) {
        std::vector<std::pair<int, spy::Message>> msgs;
        std::vector<int> joined, left;
        srv.wait_events(1000, msgs, joined, left);

        for (int fd : joined)
            printf("joined: fd=%d uid=%d\n", fd, (int)srv.peer(fd)->uid);
        for (auto& item : msgs) {
            int fd = item.first;
            spy::Message& m = item.second;
            printf("fd=%d  %s|%s\n", fd, m.type.c_str(), m.text.c_str());
            srv.broadcast({"CHAT", m.text});
        }
        for (int fd : left) printf("left: fd=%d\n", fd);
        fflush(stdout);
    }
}
