#include <iostream>
#include "structures.h"
#include "parser.h"
#include "json_output.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.json> <output.json>" << std::endl;
        return 1;
    }

    // 0. Parse JSON to get the program
    std::vector<DecodedInstructionStruct> program = parseInstructions(argv[1]);

    // Initialize processor
    ProcessorStateStruct state;
    state.reset();

    // create a JSON array to hold the log of processor states
    nlohmann::json log = nlohmann::json::array();

    // 1. Dump the state of the reset system
    // So `log` is accumulating all the snapshots. Each time we call `dumpStateIntoLog`, it adds one more entry
    dumpStateIntoLog(state, log);

    // 2. The loop for cycle-by-cycle iterations
    while (state.PC < program.size() || !state.ActiveList.empty()) {
        // propagate — compute next state
        // latch — apply next state
        // dump the state
        dumpStateIntoLog(state, log);
    }

    // 3. Save the output JSON log
    saveLog(log, argv[2]);

    return 0;
}