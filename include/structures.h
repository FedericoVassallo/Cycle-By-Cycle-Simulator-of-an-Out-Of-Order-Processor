#pragma once
#include <vector>
#include <deque>
#include <cstdint>

enum class Opcode { ADD, ADDI, SUB, MULU, DIVU, REMU };

struct DecodedInstructionStruct{
    Opcode   OpCode;
    uint32_t PC;
    uint8_t  rd;    // destination register 
    uint8_t  rs1;   // source A register 
    uint8_t  rs2;   // source B register (0-31), unused if hasImm
    int64_t  imm;   // immediate, used only if hasImm
    bool     hasImm;
};

struct ActiveListStruct{
    bool Done = false; // indicates whether the instruction has completed execution
    bool Exception = false; // indicates whether the instruction has caused an exception
    uint8_t LogicalDestination = 0; // logical destination register (0-31)
    uint8_t OldDestination = 0; // id of the physical register previously mapped to the destination architectural register
    uint32_t PC = 0; // Program Counter of the instruction
};

struct IntegerQueueStruct{
    uint8_t     DestRegister = 0;    // registro FISICO destinazione
    bool        OpAIsReady   = false;
    uint8_t     OpARegTag    = 0;    // registro fisico di srcA (per ascoltare forwarding)
    uint64_t    OpAValue     = 0;
    bool        OpBIsReady   = false;
    uint8_t     OpBRegTag    = 0;
    uint64_t    OpBValue     = 0;
    Opcode      OpCode;              // "add", "sub", "mulu", "divu", "remu", "addi"
    uint32_t    PC           = 0;
};

struct ExecutingInstructionALU{
    uint8_t     rd;  // destination register 
    bool        ExceptionCaused = false; // indicates whether the instruction has caused an exception
    uint64_t    Result = 0; // result of the ALU operation
    uint32_t    PC = 0; // Program Counter of the instruction
    uint8_t CyclesLeft = 2; // number of cycles the instruction has been executing in the ALU
};


struct ProcessorStateStruct{
    uint32_t PC = 0; // Program Counter
    uint64_t PhysicalRegisterFile[64] = {0}; // Physical Register File
    uint32_t ExceptionPC = 0; // storing the value of the PC of the instruction that triggers an exception
    bool ExceptionFlag = false; // Exception flag
    uint8_t RegisterMapTable[32] = {0}; // mapping architectural registers to physical registers
    // it has to be inizialised such that RegisterMapTable[i] = i for i in [0, 31]

    bool BackpressureFlag = false; // flag to indicate if backpressure is applied (i.e., if an exception has occurred and we need to stop fetching new instructions)
    
    bool BusyBitTable[64] = {false}; // Busy bit table for physical registers
    
    std::deque<uint8_t> FreeList; // list of free physical registers
    std::deque<ActiveListStruct> ActiveList; // Active List that has to be max size of 32
    std::vector<IntegerQueueStruct> IntegerQueue; 
    std::vector<DecodedInstructionStruct> DecodedInstructionRegister; 
    std::vector<ExecutingInstructionALU> ExecutingInstructions;

    void reset() { // reset the processor state to the initial conditions
        PC = 0;
        ExceptionPC = 0;
        ExceptionFlag = false;
        BackpressureFlag = false;
        for (int i = 0; i < 32; ++i) {
            RegisterMapTable[i] = i; // inizializza il mapping degli architectural registers
        }
        for (int i = 0; i < 64; ++i) {
            PhysicalRegisterFile[i] = 0; // inizializza il physical register file
            BusyBitTable[i] = false; // inizializza la busy bit table
        }
        FreeList.clear();
        for (int i = 32; i < 64; ++i) {
            FreeList.push_back(i); // inizializza la free list con i registri fisici disponibili
        }
        ActiveList.clear();
        IntegerQueue.clear();
        DecodedInstructionRegister.clear();
        ExecutingInstructions.clear();
    }
};