# CS-470 Homework 1

Federico Vassallo

Source Files (src/): 

- main.cpp — Entry point: reads input, runs simulation loop, writes output
- simulator.cpp — Pipeline stages: fetch, rename/dispatch, issue, execute, commit
- parser.cpp — Parses instruction strings from input JSON
- json_output.cpp — Serializes processor state to JSON each cycle

Headers (include/):

- structures.h — All data structures (ProcessorState, ActiveList, IntegerQueue, ...)
- simulator.h / parser.h / json_output.h — Function declarations
- nlohmann/ — Bundled nlohmann/json header-only library for JSON parsing and output