#include <iostream>
#include <cassert>
#include "structures.h"
#include "parser.h"
#include "json_output.h"

static const char* opcodeToString(Opcode op) {
    switch (op) {
        case Opcode::ADD:  return "add";
        case Opcode::ADDI: return "addi";
        case Opcode::SUB:  return "sub";
        case Opcode::MULU: return "mulu";
        case Opcode::DIVU: return "divu";
        case Opcode::REMU: return "remu";
    }
    return "???";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.json> <output.json>" << std::endl;
        return 1;
    }

    // ===== TEST 1: Parser =====
    std::vector<DecodedInstructionStruct> program = parseInstructions(argv[1]);

    std::cout << "=== Parser Tests ===" << std::endl;
    std::cout << "Parsed " << program.size() << " instructions:\n" << std::endl;

    for (const auto& instr : program) {
        std::cout << "  PC=" << instr.PC << "\t"
                  << opcodeToString(instr.OpCode)
                  << "\tx" << (int)instr.rd
                  << ", x" << (int)instr.rs1;
        if (instr.hasImm) {
            std::cout << ", " << instr.imm;
        } else {
            std::cout << ", x" << (int)instr.rs2;
        }
        std::cout << std::endl;
    }

    // ===== TEST 2: JSON Output of reset state =====
    std::cout << "\n=== JSON Output Tests ===" << std::endl;

    ProcessorStateStruct state;
    state.reset();

    nlohmann::json log = nlohmann::json::array();
    dumpStateIntoLog(state, log);

    assert(log.size() == 1);
    nlohmann::json snap = log[0];

    assert(snap["PC"] == 0);
    assert(snap["Exception"] == false);
    assert(snap["ExceptionPC"] == 0);
    assert(snap["RegisterMapTable"].size() == 32);
    assert(snap["FreeList"].size() == 32);
    assert(snap["FreeList"][0] == 32);
    assert(snap["PhysicalRegisterFile"].size() == 64);
    assert(snap["BusyBitTable"].size() == 64);
    assert(snap["ActiveList"].empty());
    assert(snap["IntegerQueue"].empty());
    assert(snap["DecodedPCs"].empty());
    std::cout << "  [OK] All JSON output checks passed" << std::endl;

    saveLog(log, argv[2]);
    std::cout << "\nOutput saved to " << argv[2] << std::endl;

    return 0;
}