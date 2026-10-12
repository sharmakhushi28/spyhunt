#include "communication.h"
#include <cstdio>

int main() {
	int fds[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

	spy::send_msg(fds[0], {"CHAT", "hello everyone"});
	spy::send_msg(fds[0], {"VOTE", "Player2"});

	spy::Message m;
	while (spy::recv_msg(fds[1], m) && true) {
		printf("type=%s text=%s\n", m.type.c_str(), m.text.c_str());
		if (m.type == "VOTE") break;
	}
}
