# endpoint-monitor build file
#
#   make        -> builds the ./monitor binary
#   make clean  -> removes it

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

SRC    = src/main.cpp src/process_reader.cpp src/network_tracker.cpp src/json_writer.cpp
TARGET = monitor

all: $(TARGET)

$(TARGET): $(SRC) src/process_reader.h src/network_tracker.h src/json_writer.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
