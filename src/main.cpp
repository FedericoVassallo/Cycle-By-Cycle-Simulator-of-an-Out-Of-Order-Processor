#include <iostream>
#include "structures.h"
#include "parser.h"
#include "json_output.h"
#include "simulator.h"

int main(int argc, char* argv[]) {
    if (argc < 3) { // have to provide input and output file names as arguments
        std::cerr << "Usage: " << argv[0] << " <input.json> <output.json>" << std::endl;
        return 1;
    }

    // use parser to read the input JSON file and convert it in vector of DecodedInstructionStruct that will be program memory
    std::vector<DecodedInstructionStruct> program = parseInstructions(argv[1]);

    // Initialize processor
    ProcessorStateStruct state;
    state.reset();

    // create a empty JSON array to hold all the cycle-by-cycle snapshots 
    nlohmann::json log = nlohmann::json::array();

    // dump the state of the reset system, log is accumulating all the snapshots, each dumpStateIntoLog adds one more entry
    dumpStateIntoLog(state, log); // this is cycle 0

    // loop for cycle-by-cycle iterations 
    // we only end if we finish executing all the instructions and we are not in an exception state
    while (state.PC < program.size() ||
       !state.DecodedInstructionRegister.empty() ||
       !state.ActiveList.empty() ||
       state.ExceptionFlag) {  
        
        // call the propagate that is defined in the processor.cpp and run all pipeline stages once
        propagate(state, program);
        // captures the processor state after this cycle and appends it to the log
        dumpStateIntoLog(state, log);
    }
    // once the loop ends it writes the entire accumulated log to the output JSON file
    saveLog(log, argv[2]);

    return 0;
}
