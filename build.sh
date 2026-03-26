#!/bin/bash
g++ -std=c++17 -Iinclude -o simulator \
    src/main.cpp src/parser.cpp src/json_output.cpp src/simulator.cpp
