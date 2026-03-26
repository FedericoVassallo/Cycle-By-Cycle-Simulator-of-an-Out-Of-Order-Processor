#!/bin/bash
g++ -std=c++17 -Wall -Wextra -Iinclude -o simulator \
    src/main.cpp src/parser.cpp src/json_output.cpp src/simulator.cpp
