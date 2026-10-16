CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

all: server player

server: server.cpp communication.h
	$(CXX) $(CXXFLAGS) -o server server.cpp

player: player.cpp communication.h
	$(CXX) $(CXXFLAGS) -o player player.cpp

test_frame: test_frame.cpp communication.h
	$(CXX) $(CXXFLAGS) -o test_frame test_frame.cpp

clean:
	rm -f server player test_frame *.o
