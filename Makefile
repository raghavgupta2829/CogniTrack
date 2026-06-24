# For Linux/Mac users with `make` available.
# Windows/PowerShell users: see README.md for the direct g++ command --
# you don't need `make` at all for this project.

CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -g

all: demo

demo: demo.cpp memtrack.cpp memtrack.hpp
	$(CXX) $(CXXFLAGS) -o demo demo.cpp memtrack.cpp

run: demo
	./demo

clean:
	rm -f demo *.o leaks.json

.PHONY: all run clean
