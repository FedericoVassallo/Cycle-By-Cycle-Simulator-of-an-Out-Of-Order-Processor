void fetch_decode(ProcessorStateStruct& state, const std::vector<DecodedInstructionStruct>& program_memory) {
    // basically get the instruction from the program memory at the addres of the PC,
    // thenc check if we have backpreassure applied and if not we fetch the instruction and
    // we put it in the decoded instruction register, and we update the PC

    if (state.ExceptionFlag) {
        state.PC = 0x10000;
        state.DecodedInstructionRegister.clear();
        return;
    }

    if (state.BackpressureFlag) {
        return; // If the backpressure flag is set, we do not fetch new instructions
    }

    if (state.PC >= program_memory.size()) {
        return;
    }

    uint32_t startPC = state.PC;
    int count = std::min(4, (int)(program_memory.size() - startPC));

    for (int i = 0; i < count; i++) {
        state.DecodedInstructionRegister.push_back(program_memory[startPC + i]);
    }
    state.PC = startPC + count;

    // So PC ends up one past the last instruction
    //  Index 20 is the last valid instruction, index 21 means "nothing left.
}


void commit(ProcessorStateStruct& state) { ... }   
void execute(ProcessorStateStruct& state) { ... }
void issue(ProcessorStateStruct& state) { ... }
void rename(ProcessorStateStruct& state) { ... }


void propagate(ProcessorStateStruct& state,
               const std::vector<DecodedInstructionStruct>& program) {
    commit(state);
    execute(state);
    issue(state);
    rename(state);
    fetch_decode(state, program);
}