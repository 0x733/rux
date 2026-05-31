CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -std=c11 -O3 -pthread -D_GNU_SOURCE
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -pthread
LIBS = -lcurl -lssl -lcrypto -ltorrent-rasterbar

SRC_C = src/main.c src/http_downloader.c
SRC_CXX = src/torrent_downloader.cpp
OBJ = $(SRC_C:.c=.o) $(SRC_CXX:.cpp=.o)
TARGET = rux

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
