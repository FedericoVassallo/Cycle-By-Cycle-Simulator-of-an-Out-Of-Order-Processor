#include "simulator.h"
#include "structures.h"
#include <vector>
#include <algorithm>
#include <cstdint>

void fetchDecode(const ProcessorStateStruct& current, 
                 ProcessorStateStruct& next,
                 const std::vector<DecodedInstructionStruct>& program_memory) {
    // basically get the instruction from the program memory at the addres of the PC,
    // then check if we have backpressure applied and if not we fetch the instruction and
    // we put it in the decoded instruction register, and we update the PC

    if (next.ExceptionFlag) {
        // read from current: ExceptionFlag is register-like, set by Commit last cycle
        next.PC = 0x10000;
        next.DecodedInstructionRegister.clear();
        return;
    }

    if (next.BackpressureFlag) {
        // read from next: Rename may have set this earlier in the same propagate cycle
        return;
    }

    if (next.PC >= program_memory.size()) {
        // read from next: PC might have been updated by other stages this cycle
        return;
    }

    uint32_t startPC = next.PC;
    int count = std::min(4, (int)(program_memory.size() - startPC));

    for (int i = 0; i < count; i++) {
        next.DecodedInstructionRegister.push_back(program_memory[startPC + i]);
    }
    next.PC = startPC + count;

    // So PC ends up one past the last instruction
    // Index 20 is the last valid instruction, index 21 means "nothing left."
}

static void renameDispatch(const ProcessorStateStruct& current, ProcessorStateStruct& next) {
    if (next.ExceptionFlag) return;

    int DIRSize = next.DecodedInstructionRegister.size();
    if (DIRSize == 0) {
        return;
    }

    if ((next.FreeList.size() < (size_t)DIRSize) ||
        (next.ActiveList.size() + DIRSize > 32) ||
        (next.IntegerQueue.size() + DIRSize > 32)) {
        next.BackpressureFlag = true;
        return;
    }

    for (int i = 0; i < DIRSize; i++) {
        DecodedInstructionStruct instr = next.DecodedInstructionRegister[i];

        // Allocate new physical register
        uint8_t newPhysReg = next.FreeList.front();
        next.FreeList.pop_front();

        // Look up sources BEFORE updating the map for rd
        uint8_t physRS1 = next.RegisterMapTable[instr.rs1];
        uint8_t physRS2 = next.RegisterMapTable[instr.rs2]; // unused if hasImm, but safe

        // Active List entry
        ActiveListStruct ActiveListEntry;
        ActiveListEntry.Done = false;
        ActiveListEntry.Exception = false;
        ActiveListEntry.PC = instr.PC;
        ActiveListEntry.OldDestination = next.RegisterMapTable[instr.rd];
        ActiveListEntry.LogicalDestination = instr.rd;
        next.ActiveList.push_back(ActiveListEntry);

        // Update map and busy bit AFTER reading sources
        next.RegisterMapTable[instr.rd] = newPhysReg;
        next.BusyBitTable[newPhysReg] = true;

        // Integer Queue entry
        IntegerQueueStruct IntegerQueueEntry;
        IntegerQueueEntry.DestRegister = newPhysReg;
        IntegerQueueEntry.OpCode = instr.OpCode;
        IntegerQueueEntry.PC = instr.PC;

        // Operand A
        IntegerQueueEntry.OpARegTag = physRS1;
        IntegerQueueEntry.OpAIsReady = !next.BusyBitTable[physRS1];
        if (IntegerQueueEntry.OpAIsReady) {
            IntegerQueueEntry.OpARegTag = 0;  // tag unused when ready
            IntegerQueueEntry.OpAValue = next.PhysicalRegisterFile[physRS1];
        } else {
            IntegerQueueEntry.OpARegTag = physRS1;
            IntegerQueueEntry.OpAValue = 0;
        }

        // Operand B
        if (instr.hasImm) {
            IntegerQueueEntry.OpBIsReady = true;
            IntegerQueueEntry.OpBRegTag = 0;
            IntegerQueueEntry.OpBValue = static_cast<uint64_t>(instr.imm);
        } else {
            IntegerQueueEntry.OpBIsReady = !next.BusyBitTable[physRS2];
            if (IntegerQueueEntry.OpBIsReady) {
                IntegerQueueEntry.OpBRegTag = 0;  // tag unused when ready
                IntegerQueueEntry.OpBValue = next.PhysicalRegisterFile[physRS2];
            } else {
                IntegerQueueEntry.OpBRegTag = physRS2;
                IntegerQueueEntry.OpBValue = 0;
            }
        }

        next.IntegerQueue.push_back(IntegerQueueEntry);
    }

    next.DecodedInstructionRegister.clear();
    next.BackpressureFlag = false;
}

void issue(const ProcessorStateStruct& current, ProcessorStateStruct& next) { 

    // the ALU can accept up to 4 instruction per cycle
    int issueLeft = 4;

    // if we have more than the issueLeft we issue the ones that are the oldest so with lower PC
    // since the integer queue is in order of dispatching, the oldest are at the beginning of the queue

    // read from next: Execute already updated ready bits via forwarding earlier this cycle
    for (int i = 0; i < (int)next.IntegerQueue.size() && issueLeft > 0; i++) {
        if (next.IntegerQueue[i].OpAIsReady && next.IntegerQueue[i].OpBIsReady) {
            // if the instruction is ready to be issued, we move it to the executing instructions list
            ExecutingInstructionALU ExecutingInstruction;
            ExecutingInstruction.rd = next.IntegerQueue[i].DestRegister;
            ExecutingInstruction.OpCode = next.IntegerQueue[i].OpCode;
            ExecutingInstruction.Result = 0; // the result will be computed in the execute stage
            ExecutingInstruction.PC = next.IntegerQueue[i].PC;
            ExecutingInstruction.OpAValue = next.IntegerQueue[i].OpAValue;
            ExecutingInstruction.OpBValue = next.IntegerQueue[i].OpBValue;

            next.ExecutingInstructions.push_back(ExecutingInstruction);
            // remove the instruction from the integer queue
            next.IntegerQueue.erase(next.IntegerQueue.begin() + i);
            i--; // adjust index after erasing, since after erasing we will shift back the indexes
            issueLeft--;
        }
    }
}

void execute(const ProcessorStateStruct& current, ProcessorStateStruct& next) {
    // Process each instruction currently in the ALUs
    for (int i = 0; i < (int)next.ExecutingInstructions.size(); i++) {
        next.ExecutingInstructions[i].CyclesLeft--;

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

            // Always mark Done in ActiveList
            for (int j = 0; j < (int)next.ActiveList.size(); j++) {
                if (next.ActiveList[j].PC == next.ExecutingInstructions[i].PC &&
                    !next.ActiveList[j].Done) {
                    next.ActiveList[j].Done = true;
                    next.ActiveList[j].Exception = next.ExecutingInstructions[i].ExceptionCaused;
                    break;
                }
            }

            // Only write result and clear busy bit if no exception
            if (!next.ExecutingInstructions[i].ExceptionCaused) {
                next.PhysicalRegisterFile[next.ExecutingInstructions[i].rd] =
                    next.ExecutingInstructions[i].Result;
                next.BusyBitTable[next.ExecutingInstructions[i].rd] = false;

                // Only forward to IQ if no exception
                for (int j = 0; j < (int)next.IntegerQueue.size(); j++) {
                    if (!next.IntegerQueue[j].OpAIsReady &&
                        next.IntegerQueue[j].OpARegTag == next.ExecutingInstructions[i].rd) {
                        next.IntegerQueue[j].OpAIsReady = true;
                        next.IntegerQueue[j].OpARegTag = 0;  // clear tag when ready
                        next.IntegerQueue[j].OpAValue = next.ExecutingInstructions[i].Result;
                    }
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

    // Remove completed instructions (CyclesLeft == 0) from the executing list
    // We iterate backwards to avoid index issues when erasing
    for (int i = (int)next.ExecutingInstructions.size() - 1; i >= 0; i--) {
        if (next.ExecutingInstructions[i].CyclesLeft == 0) {
            next.ExecutingInstructions.erase(next.ExecutingInstructions.begin() + i);
        }
    }
}

void commit(const ProcessorStateStruct& current, ProcessorStateStruct& next) {

    if (current.ExceptionFlag) {
    // If ActiveList is already empty, just clear the flag this cycle
    if (next.ActiveList.empty()) {
        next.ExceptionFlag = false;
        return;
    }
    // Otherwise rollback up to 4 instructions
    int rollbackCount = std::min(4, (int)next.ActiveList.size());
    for (int i = 0; i < rollbackCount; i++) {
        ActiveListStruct entry = next.ActiveList.back();
        next.ActiveList.pop_back();
        uint8_t newPhysReg = next.RegisterMapTable[entry.LogicalDestination];
        next.RegisterMapTable[entry.LogicalDestination] = entry.OldDestination;
        next.FreeList.push_back(newPhysReg);
        next.BusyBitTable[newPhysReg] = false;
    }
    // Do NOT clear ExceptionFlag here — wait until next cycle when AL is empty
    return;
}

    // ===== Normal Commit Mode =====
    // Retire up to 4 instructions from the FRONT of the Active List (oldest first)

    int commitCount = 0;

    while (commitCount < 4 && !next.ActiveList.empty()) {
        ActiveListStruct front = next.ActiveList.front();

        if (!front.Done) {
            break; // instruction not completed yet, stop committing
        }

        if (front.Exception) {
            next.ExceptionFlag = true;
            next.ExceptionPC = front.PC;
            next.IntegerQueue.clear();
            next.ExecutingInstructions.clear();
            // Do NOT remove the excepting instruction or free its register here
            // Rollback will handle it next cycle
            break;
        }

        // Normal retirement
        // The old physical register is no longer needed — return to free list
        // For normal commit, earlier instructions (in program order) go EARLIER in the free list
        next.FreeList.push_back(front.OldDestination);
        next.ActiveList.pop_front();
        commitCount++;
    }
}

void propagate(ProcessorStateStruct& state,
               const std::vector<DecodedInstructionStruct>& program) {
    // Make a copy — this becomes the "next state"
    ProcessorStateStruct next = state;
    next.BackpressureFlag = false;
    // All stages run in reverse pipeline order
    // Each reads from 'state' (current) for register-like values
    // and from 'next' for queues (asynchronous-read)
    commit(state, next);
    execute(state, next);
    issue(state, next);
    renameDispatch(state, next);
    fetchDecode(state, next, program);

    // Latch — replace current with next
    state = next;
}