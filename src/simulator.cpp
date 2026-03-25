void fetchDecode(ProcessorStateStruct& state, const std::vector<DecodedInstructionStruct>& program_memory) {
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

void renameDispatch(ProcessorStateStruct& state) { 

    // Check if there are enough physical registers, enough entries in the Active List, and enough entries
    // in the Integer Queue. If not, apply back pressure to the previous stage

    int DIRSize = state.DecodedInstructionRegister.size();

    if (DIRSize == 0) {
        return; // No instructions to rename and dispatch
    }    

    if ((state.FreeList.size() < DIRSize) || (state.ActiveList.size() + DIRSize > 32) || (state.IntegerQueue.size() + DIRSize > 32)) {
        state.BackpressureFlag = true; // apply back pressure and we don't dispatch any instruction
        return;
    }

    // If there are enough physical resources, rename the instructions decoded from the previous stage and
    // update the Register Map Table and Free List accordingly

    for (int i = 0; i < DIRSize; i++) {
        // Allocate a new physical register from the free list
        uint8_t newPhysReg = state.FreeList.front();
        state.FreeList.pop_front();

        // for each instruction in the decoded instruction register
        // we put it in the active list and in the integer queue
        ActiveListStruct ActiveListEntry;
        ActiveListEntry.Done = false;
        ActiveListEntry.Exception = false;
        ActiveListEntry.PC = state.DecodedInstructionRegister[i].PC;
        // save the old mapping of the destination register in the active list entry,
        // so that we can restore it in case of an exception
        ActiveListEntry.OldDestination = state.RegisterMapTable[state.DecodedInstructionRegister[i].rd];
        ActiveListEntry.LogicalDestination = state.DecodedInstructionRegister[i].rd;

        state.ActiveList.push_back(ActiveListEntry);

        // Update the register map table to point to the new physical register
        state.RegisterMapTable[state.DecodedInstructionRegister[i].rd] = newPhysReg;
        // Mark the new physical register as busy (it will be written by the ALU later)
        state.BusyBitTable[newPhysReg] = true;

        IntegerQueueStruct IntegerQueueEntry;
        IntegerQueueEntry.DestRegister = newPhysReg;
        IntegerQueueEntry.OpCode = state.DecodedInstructionRegister[i].OpCode;
        IntegerQueueEntry.PC = state.DecodedInstructionRegister[i].PC;

        // Operand A: always a register (rs1)
        uint8_t physRS1 = state.RegisterMapTable[state.DecodedInstructionRegister[i].rs1];
        IntegerQueueEntry.OpARegTag = physRS1;
        // ready means NOT busy
        IntegerQueueEntry.OpAIsReady = !state.BusyBitTable[physRS1];
        if (IntegerQueueEntry.OpAIsReady) {
            IntegerQueueEntry.OpAValue = state.PhysicalRegisterFile[physRS1];
        } else {
            IntegerQueueEntry.OpAValue = 0;
        }

        // Operand B: register (rs2) or immediate depending on instruction type
        if (state.DecodedInstructionRegister[i].hasImm) {
            // for addi, the second operand is an immediate value, always ready
            IntegerQueueEntry.OpBIsReady = true;
            IntegerQueueEntry.OpBRegTag = 0;
            IntegerQueueEntry.OpBValue = state.DecodedInstructionRegister[i].imm;
        } else {
            uint8_t physRS2 = state.RegisterMapTable[state.DecodedInstructionRegister[i].rs2];
            IntegerQueueEntry.OpBRegTag = physRS2;
            // ready means NOT busy
            IntegerQueueEntry.OpBIsReady = !state.BusyBitTable[physRS2];
            if (IntegerQueueEntry.OpBIsReady) {
                IntegerQueueEntry.OpBValue = state.PhysicalRegisterFile[physRS2];
            } else {
                IntegerQueueEntry.OpBValue = 0;
            }
        }
        state.IntegerQueue.push_back(IntegerQueueEntry);
    }
    state.DecodedInstructionRegister.clear(); // clear the decoded instruction register after dispatching
    state.BackpressureFlag = false; // clear back pressure flag if we were able to dispatch
}

void issue(ProcessorStateStruct& state) { 

    // the ALU can accept up to 4 instruction per cycle
    int issueLeft = 4;

    // if we have more than the issueLeft we issue the ones that are the oldest so with higher PC
    // since the integer queue is in order of dispatching, the oldest are at the beginning of the queue

    for (int i = 0; i < state.IntegerQueue.size() && issueLeft > 0; i++) {
        if (state.IntegerQueue[i].OpAIsReady && state.IntegerQueue[i].OpBIsReady) {
            // if the instruction is ready to be issued, we move it to the executing instructions list
            ExecutingInstructionALU ExecutingInstruction;
            ExecutingInstruction.rd = state.IntegerQueue[i].DestRegister;
            ExecutingInstruction.OpCode = state.IntegerQueue[i].OpCode;
            ExecutingInstruction.Result = 0; // the result will be computed in the execute stage
            ExecutingInstruction.PC = state.IntegerQueue[i].PC;
            ExecutingInstruction.OpAValue = state.IntegerQueue[i].OpAValue;
            ExecutingInstruction.OpBValue = state.IntegerQueue[i].OpBValue;

            state.ExecutingInstructions.push_back(ExecutingInstruction);
            // remove the instruction from the integer queue
            state.IntegerQueue.erase(state.IntegerQueue.begin() + i);
            i--; // adjust index after erasing, since after erasing we will shift back the indexes
            issueLeft--;
        }
    }

    // still to be tought about the fact that it ask:
    // "The Issue unit can issue both instructions with all operands noted as ready in the Integer Queue and ones 
    // with operands not yet ready but provided by a forwarding path"
    // But apparently it seems that by using the order of the calls that are in the propagate function 
    // basically we will have to forwarding value in the execute function so that when we call the issue
    // the forwarding path should be already updated? 

    // ask TA about this fact that they do not operate actually in parallel in a simulator but seuqntially
}

void execute(ProcessorStateStruct& state) {



}

void commit(ProcessorStateStruct& state) {




}   

void propagate(ProcessorStateStruct& state,
               const std::vector<DecodedInstructionStruct>& program) {
    commit(state);
    execute(state);
    issue(state);
    renameDispatch(state);
    fetchDecode(state, program);
}