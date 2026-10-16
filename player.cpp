#include "communication.h"
#include <iostream>

int main() {
    spy::Client c;
    if (!c.connect_to("/tmp/spyhunt/spyhunt.sock")) { perror("connect"); return 1; }
    printf("================================\n           SPY HUNT\n================================\n");

    while (true) {
        int ev = c.wait();

        if (ev & spy::Client::KEYBOARD) {
            std::string line;
            if (!std::getline(std::cin, line)) break;
            c.send({"CHAT", line});
        }
        if (ev & spy::Client::SERVER) {
            spy::Message m;
            if (!c.recv(m)) { printf("server closed\n"); break; }
            if (m.type == "ROLE")
                printf("\n>>> YOUR SECRET ROLE: %s <<<\n\n", m.text.c_str());
            else if (m.type == "RESULT")
                printf("\n[INVESTIGATION RESULT]\n%s\n\n", m.text.c_str());
            else if (m.type == "PRIVATE") {
                size_t bar = m.text.find('|');
                printf("[PRIVATE - %s] %s\n", m.text.substr(0, bar).c_str(),
                       m.text.substr(bar + 1).c_str());
            }
            else if (m.type == "ERROR")
                printf("[ERROR] %s\n", m.text.c_str());
            else
                printf("%s\n", m.text.c_str());
        }
    }
}
