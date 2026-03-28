// version 1 of the exectute function

void execute(const ProcessorStateStruct& current, ProcessorStateStruct& next) {
    // Process each instruction currently in the ALUs
    for (int i = 0; i < (int)next.ExecutingInstructions.size(); i++) {
        next.ExecutingInstructions[i].CyclesLeft--; // decrease the cycles left

        if (next.ExecutingInstructions[i].CyclesLeft == 0) {
            // instruction is finishing this cycle

            // Compute the result based on the opcode
            switch (next.ExecutingInstructions[i].OpCode) {
                case Opcode::ADD: 
                case Opcode::ADDI:
                    next.ExecutingInstructions[i].Result = 
                        next.ExecutingInstructions[i].OpAValue + next.ExecutingInstructions[i].OpBValue;
                    break;
                case Opcode::SUB:
                    next.ExecutingInstructions[i].Result = 
                        next.ExecutingInstructions[i].OpAValue - next.ExecutingInstructions[i].OpBValue;
                    break;
                case Opcode::MULU:
                    next.ExecutingInstructions[i].Result = 
                        next.ExecutingInstructions[i].OpAValue * next.ExecutingInstructions[i].OpBValue;
                    break;
                case Opcode::DIVU:
                    if (next.ExecutingInstructions[i].OpBValue == 0) {
                        next.ExecutingInstructions[i].ExceptionCaused = true;
                        next.ExecutingInstructions[i].Result = 0; // won't be committed
                    } else {
                        next.ExecutingInstructions[i].Result = 
                            next.ExecutingInstructions[i].OpAValue / next.ExecutingInstructions[i].OpBValue;
                    }
                    break;
                case Opcode::REMU:
                    if (next.ExecutingInstructions[i].OpBValue == 0) {
                        next.ExecutingInstructions[i].ExceptionCaused = true;
                        next.ExecutingInstructions[i].Result = 0; // won't be committed
                    } else {
                        next.ExecutingInstructions[i].Result = 
                            next.ExecutingInstructions[i].OpAValue % next.ExecutingInstructions[i].OpBValue;
                    }
                    break;
            }

            // always mark done in ActiveList even with exception 
            // search in active list to find the instr that that just finished executing
            for (int j = 0; j < (int)next.ActiveList.size(); j++) {
                if (next.ActiveList[j].PC == next.ExecutingInstructions[i].PC &&
                    !next.ActiveList[j].Done) { // also check it is not already marked done (but shouldn't be possible anyway)
                    next.ActiveList[j].Done = true;
                    next.ActiveList[j].Exception = next.ExecutingInstructions[i].ExceptionCaused; // set if it caused exception
                    break;
                }
            }

            // we write result and clear busy bit if it did not cause exception 
            if (!next.ExecutingInstructions[i].ExceptionCaused) {
                next.PhysicalRegisterFile[next.ExecutingInstructions[i].rd] =
                    next.ExecutingInstructions[i].Result;
                next.BusyBitTable[next.ExecutingInstructions[i].rd] = false;

                // we update in the IQ the instruction that are waiting for this result as operand
                for (int j = 0; j < (int)next.IntegerQueue.size(); j++) {
                    // if their opA is not ready and wait for this register
                    if (!next.IntegerQueue[j].OpAIsReady &&
                        next.IntegerQueue[j].OpARegTag == next.ExecutingInstructions[i].rd) {
                        next.IntegerQueue[j].OpAIsReady = true;
                        next.IntegerQueue[j].OpARegTag = 0;  // clear tag when ready
                        next.IntegerQueue[j].OpAValue = next.ExecutingInstructions[i].Result;
                    }
                    // if their opB is not ready and wait for this register
                    if (!next.IntegerQueue[j].OpBIsReady &&
                        next.IntegerQueue[j].OpBRegTag == next.ExecutingInstructions[i].rd) {
                        next.IntegerQueue[j].OpBIsReady = true;
                        next.IntegerQueue[j].OpBRegTag = 0;  // clear tag when ready
                        next.IntegerQueue[j].OpBValue = next.ExecutingInstructions[i].Result;
                    }
                }
            }
        }
    }

    // remove completed instructions (CyclesLeft == 0) from the executing list (also the one with exception)
    // iterate backwards because when erasig the index shift so it's easier this way to avoid skip
    for (int i = (int)next.ExecutingInstructions.size() - 1; i >= 0; i--) {
        if (next.ExecutingInstructions[i].CyclesLeft == 0) {
            next.ExecutingInstructions.erase(next.ExecutingInstructions.begin() + i);
        }
    }
}