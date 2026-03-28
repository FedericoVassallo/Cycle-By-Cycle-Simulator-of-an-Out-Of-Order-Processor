#include "json_output.h"
#include <fstream>

// the dumpStateIntoLog takes a ProcessorStateStruct and serializes it into a JSON snapshot. 
// it get called once per cycle to record the state.

// instead the saveLog handles writing the final JSON array to the output file.

// helper function to convert Opcode enum to string for the JSON output
static const char* opcodeToString(Opcode op) {
    switch (op) {
        case Opcode::ADD:
        case Opcode::ADDI:  // addi is stored as "add" in the IQ
            return "add"; 
        case Opcode::SUB:  return "sub";
        case Opcode::MULU: return "mulu";
        case Opcode::DIVU: return "divu";
        case Opcode::REMU: return "remu";
    }
    return "???";
}

void dumpStateIntoLog(const ProcessorStateStruct& state, nlohmann::json& log) {
    // snapshot is a JSON object that will hold the current state of the processor for this cycle
    nlohmann::json snapshot;

    // assign values using []  
    snapshot["PC"] = state.PC;
    snapshot["ExceptionPC"] = state.ExceptionPC;
    snapshot["Exception"] = state.ExceptionFlag;

    // PhysicalRegisterFile is an array array of 64 integers
    snapshot["PhysicalRegisterFile"] = nlohmann::json::array();
    for (int i = 0; i < 64; ++i) {
        snapshot["PhysicalRegisterFile"].push_back(state.PhysicalRegisterFile[i]);
    }

    // RegisterMapTable is an array of 32 integers
    snapshot["RegisterMapTable"] = nlohmann::json::array();
    for (int i = 0; i < 32; ++i) {
        snapshot["RegisterMapTable"].push_back(state.RegisterMapTable[i]);
    }

    // BusyBitTable is an array of 64 booleans
    snapshot["BusyBitTable"] = nlohmann::json::array();
    for (int i = 0; i < 64; ++i) {
        snapshot["BusyBitTable"].push_back(state.BusyBitTable[i]);
    }

    // FreeList is an array of register ids
    snapshot["FreeList"] = nlohmann::json::array();
    for (uint8_t reg : state.FreeList) {
        snapshot["FreeList"].push_back(reg);
    }

    // DecodedPCs only the PCs of the instr in the DecodedInstructionRegister
    snapshot["DecodedPCs"] = nlohmann::json::array();
    for (const auto& instr : state.DecodedInstructionRegister) {
        snapshot["DecodedPCs"].push_back(instr.PC);
    }

    // save the entry of the ActiveList 
    snapshot["ActiveList"] = nlohmann::json::array();
    for (const auto& entry : state.ActiveList) {
        nlohmann::json obj;
        obj["Done"]               = entry.Done;
        obj["Exception"]          = entry.Exception;
        obj["LogicalDestination"] = entry.LogicalDestination;
        obj["OldDestination"]     = entry.OldDestination;
        obj["PC"]                 = entry.PC;
        snapshot["ActiveList"].push_back(obj);
    }

    // save the entry of the IntegerQueue 
    snapshot["IntegerQueue"] = nlohmann::json::array();
    for (const auto& entry : state.IntegerQueue) {
        nlohmann::json obj;
        obj["DestRegister"] = entry.DestRegister;
        obj["OpAIsReady"]   = entry.OpAIsReady;
        obj["OpARegTag"]    = entry.OpARegTag;
        obj["OpAValue"]     = entry.OpAValue;
        obj["OpBIsReady"]   = entry.OpBIsReady;
        obj["OpBRegTag"]    = entry.OpBRegTag;
        obj["OpBValue"]     = entry.OpBValue;
        obj["OpCode"]       = opcodeToString(entry.OpCode);
        obj["PC"]           = entry.PC;
        snapshot["IntegerQueue"].push_back(obj);
    }

    log.push_back(snapshot);
}

void saveLog(const nlohmann::json& log, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open output file: " + filename);
    }
    file << log.dump(4);
}