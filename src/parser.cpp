#include "parser.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

///////////////   this parser.cpp is to read the in JSON file and convert it into         /////////////
/////////////// a vector of DecodedInstructionStruct that I will n use in the simulator.  /////////////

// helper function to convert string to Opcode enum (use const to make sure not modified)
static Opcode stringToOpcode(const std::string& s) { 
    if (s == "add")  return Opcode::ADD;
    if (s == "addi") return Opcode::ADDI;
    if (s == "sub")  return Opcode::SUB;
    if (s == "mulu") return Opcode::MULU;
    if (s == "divu") return Opcode::DIVU;
    if (s == "remu") return Opcode::REMU;
    throw std::runtime_error("Unknown opcode: " + s);
}

static uint8_t stringToRegister(const std::string& s) {
    if (s.empty() || s[0] != 'x')
        throw std::runtime_error("Invalid register name: " + s);
    int regNum = std::stoi(s.substr(1));
    if (regNum < 0 || regNum > 31)
        throw std::runtime_error("Register number out of range: " + s);
    return static_cast<uint8_t>(regNum);
}

static DecodedInstructionStruct parseLine(const std::string& line, uint32_t pc) {
    // This copies the string and replaces every comma with a space
    std::string clean = line;
    std::replace(clean.begin(), clean.end(), ',', ' ');

    // Now we can use a stringstream to parse the cleaned line
    std::istringstream iss(clean);
    std::string opcodeStr, destStr, src1Str, src2OrImmStr;
    iss >> opcodeStr >> destStr >> src1Str >> src2OrImmStr;

    // Convert each token using the helpers from before.
    DecodedInstructionStruct instr;
    instr.PC     = pc;
    instr.OpCode = stringToOpcode(opcodeStr);
    instr.rd     = stringToRegister(destStr);
    instr.rs1    = stringToRegister(src1Str);

    // we need to check if the instruction is an immediate type (ADDI) or not,
    // this change the way we interpret the last token (src2OrImmStr) as either an immediate value or a register
    if (instr.OpCode == Opcode::ADDI) {
        instr.hasImm = true;
        instr.imm    = std::stoll(src2OrImmStr);
        instr.rs2    = 0;
    } else {
        instr.hasImm = false;
        instr.imm    = 0;
        instr.rs2    = stringToRegister(src2OrImmStr);
    }

    return instr;
}

// this function reads the JSON file and calls parseLine on each entry
std::vector<DecodedInstructionStruct> parseInstructions(const std::string& filename) {
    // the C++ equivalent of FILE* f = fopen(filename, "r"). The is_open() check is like checking f == NULL
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    // we use the nlohmann::json library to parse the JSON file. The library will read the entire file into memory and parse it as a JSON object.
    nlohmann::json j;
    file >> j;

    // check 
    if (!j.is_array()) {
        throw std::runtime_error("JSON root must be an array");
    }

    // we loop each element 
    std::vector<DecodedInstructionStruct> program;
    for (uint32_t i = 0; i < j.size(); ++i) {
        program.push_back(parseLine(j[i].get<std::string>(), i));
    }

    return program;
}



