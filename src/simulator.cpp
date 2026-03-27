#include "simulator.h"
#include "structures.h"
#include <vector>
#include <algorithm>
#include <cstdint>

void fetchDecode(const ProcessorStateStruct& current, 
                 ProcessorStateStruct& next,
                 const std::vector<DecodedInstructionStruct>& program_memory) {
    /* basically get the instruction from the program memory at the addres of the PC,
       then check if we have backpressure applied and if not we fetch the instruction and
       we put it in the decoded instruction register, and we update the PC  */

    // if we see that an exception is raised we need to clear the decoded instruction register 
    // and set the PC to the exception handler address (0x10000) and we return
    if (next.ExceptionFlag) {
        next.PC = 0x10000;
        next.DecodedInstructionRegister.clear(); 
        return;
    }

    // See if the next stage has applied backpreassure, and if so don't fetch new instructions this cycle 
    if (next.BackpressureFlag) {
        return;
    }

    // read if the PC is out of bounds (read from next but would be the same if read from current since it has not been updated yet) 
    if (next.PC >= program_memory.size()) {
        return;
    }

    uint32_t startPC = next.PC;
    // If we have less than 4 instructions left in the program memory, we only fetch the remaining instructions (by taking the min of 4 and the number of instructions left)
    int count = std::min(4, (int)(program_memory.size() - startPC));

    //  take the instruction and we put them in the decoded instruction register (in the end of the vector)
    for (int i = 0; i < count; i++) {
        next.DecodedInstructionRegister.push_back(program_memory[startPC + i]);
    }
    next.PC = startPC + count; // update the PC to point to the next instruction to fetch in the next cycle

    // So in the end the PC ends up one instruction after the last being inserted in the decoded instruction register, 
    // which means that if we have fetched the last instruction (index 20) the PC will be 21,
    // and in the next cycle the fetch will see that the PC is > 20 and will not fetch anymore
}

static void renameDispatch(const ProcessorStateStruct& current, ProcessorStateStruct& next) {
    // if theree is an exception we stop dispatching and wait for the commit
    if (next.ExceptionFlag){
        return;
    } 

    // save the sizer of the decoded instruction register that will be useful to check if we have space in the structures
    int DIRSize = next.DecodedInstructionRegister.size();
    if (DIRSize == 0) { // if it is 0, nothing to dispatch so just return 
        return;
    }

    // here there have to be enough space in the FreeList, ActiveList and IntegerQueue otherwise we apply backpressure and the instruction stays in the DIR
    if ((next.FreeList.size() < (size_t)DIRSize) ||
        (next.ActiveList.size() + DIRSize > 32) ||
        (next.IntegerQueue.size() + DIRSize > 32)) {
        next.BackpressureFlag = true;
        return;
    }

    // at this point there is space so rename the instructions decoded from the 
    // previous stage and update the Register Map Table and Free List
    for (int i = 0; i < DIRSize; i++) {
        DecodedInstructionStruct instr = next.DecodedInstructionRegister[i];

        // Allocate new physical register by taking the first one from the free list
        uint8_t newPhysReg = next.FreeList.front();
        next.FreeList.pop_front(); // remove it from the free list

        // look where the rs1 and rs2 are mapped in the register map table (the physical register that we have to read for each source operand) 
        uint8_t physRS1 = next.RegisterMapTable[instr.rs1]; 
        uint8_t physRS2 = next.RegisterMapTable[instr.rs2]; // unused if hasImm, because it would mean instr.rs2 = 0; but anyway we will not read from the physRS2

        // Active List entry
        ActiveListStruct ActiveListEntry;
        ActiveListEntry.Done = false;
        ActiveListEntry.Exception = false;
        ActiveListEntry.PC = instr.PC;
        ActiveListEntry.OldDestination = next.RegisterMapTable[instr.rd]; // the physical register that was mapped to rd before this instruction renamed it.
        ActiveListEntry.LogicalDestination = instr.rd; // simply the architectural destination register (the one in the Opcode of the instruction). Usefull for exception recovery
        next.ActiveList.push_back(ActiveListEntry); // put the entry at the end

        // Update map and busy bit and the ph register file
        next.RegisterMapTable[instr.rd] = newPhysReg;
        next.BusyBitTable[newPhysReg] = true;

        // Integer Queue entry
        IntegerQueueStruct IntegerQueueEntry;
        IntegerQueueEntry.DestRegister = newPhysReg; // the ph register allocated for the result of this instruction
        IntegerQueueEntry.OpCode = instr.OpCode;
        IntegerQueueEntry.PC = instr.PC;

        // Operand A
        IntegerQueueEntry.OpARegTag = physRS1; // ph register tag for operand A
        IntegerQueueEntry.OpAIsReady = !next.BusyBitTable[physRS1]; // we check if the register is ready by the busyBit
        if (IntegerQueueEntry.OpAIsReady) {
            IntegerQueueEntry.OpARegTag = 0;  // tag unused when ready
            IntegerQueueEntry.OpAValue = next.PhysicalRegisterFile[physRS1];
        } else {
            IntegerQueueEntry.OpARegTag = physRS1;
            IntegerQueueEntry.OpAValue = 0; // value unused when not ready
        }

        // Operand B (this one can be an immediate)
        if (instr.hasImm) {
            IntegerQueueEntry.OpBIsReady = true;
            IntegerQueueEntry.OpBRegTag = 0; // again not used as above since the operand is an immediate
            IntegerQueueEntry.OpBValue = static_cast<uint64_t>(instr.imm); // cast to uint64 
        } else {    
            IntegerQueueEntry.OpBIsReady = !next.BusyBitTable[physRS2]; // check the busy table
            if (IntegerQueueEntry.OpBIsReady) {
                IntegerQueueEntry.OpBRegTag = 0;  // tag unused when ready
                IntegerQueueEntry.OpBValue = next.PhysicalRegisterFile[physRS2]; // we read the value
            } else {
                IntegerQueueEntry.OpBRegTag = physRS2; 
                IntegerQueueEntry.OpBValue = 0; // value unused when not ready
            }
        }

        next.IntegerQueue.push_back(IntegerQueueEntry); // we put it at the end of the queue
    }

    next.DecodedInstructionRegister.clear();
    next.BackpressureFlag = false;


    /* Since execute() runs before renameDispatch() the forwarding is handeled
       in the propagate() call, because the execute operate on 'next', any physical register
       written by the ALU this cycle is already visible in next.PhysicalRegisterFile and
       next.BusyBitTable by when they are checked in the renameDispatch() stage (I have to remember to ask TA if it's fine) */
}

void issue(const ProcessorStateStruct& current, ProcessorStateStruct& next) { 

    // the ALU can accept up to 4 instruction per cycle
    int issueLeft = 4;

    // if we have more than the issueLeft we issue the ones that are the oldest so with lower PC
    // since the integer queue is in order of dispatching, the oldest are at the beginning of the queue

    // So when issue scans the IQ looking for ready instructions, it already sees the forwarded ready bits that execute just set. 
    // So it can issue instructions that are given by the forwarding in the same cycle  
    for (int i = 0; i < (int)next.IntegerQueue.size() && issueLeft > 0; i++) {
        if (next.IntegerQueue[i].OpAIsReady && next.IntegerQueue[i].OpBIsReady) {
            // (the update of Opa/Opb isready and their values is in the execute stage)
            // if the instruction is ready to be issued, move it to the executing instructions list
            ExecutingInstructionALU ExecutingInstruction;
            ExecutingInstruction.rd = next.IntegerQueue[i].DestRegister;
            ExecutingInstruction.OpCode = next.IntegerQueue[i].OpCode;
            ExecutingInstruction.Result = 0; // the result is not computed yet
            ExecutingInstruction.PC = next.IntegerQueue[i].PC;
            ExecutingInstruction.OpAValue = next.IntegerQueue[i].OpAValue;
            ExecutingInstruction.OpBValue = next.IntegerQueue[i].OpBValue;
            ExecutingInstruction.CyclesLeft = 2; // it takes two cycles in the ALU
            ExecutingInstruction.ExceptionCaused = false; 
            // we put it in the list
            next.ExecutingInstructions.push_back(ExecutingInstruction);
            // remove the instruction from the integer queue
            next.IntegerQueue.erase(next.IntegerQueue.begin() + i);
            i--; // adjust index after erasing, since after erasing we will shift back the indexes
            issueLeft--; // to track that we can only issue up to 4 instructions per cycle
        }
    }
}

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

void commit(const ProcessorStateStruct& current, ProcessorStateStruct& next) {

    // we check if we are in an exception state
    if (current.ExceptionFlag) {
    // if ActiveList is already empty, just clear the flag this cycle (means rolled back already done) 
    if (next.ActiveList.empty()) { // could also check current
        next.ExceptionFlag = false; // exit the exception mode 
        return;
    }
    // if is not empty rollback up to 4 instructions per cycle (less than 4 if we have less in ActiveList)
    int rollbackCount = std::min(4, (int)next.ActiveList.size());
    for (int i = 0; i < rollbackCount; i++) {
        // take the youngest instruction in the ActiveList (the one at the end) and roll it back
        // start from the end because the renaming has to be undone in reverse
        ActiveListStruct entry = next.ActiveList.back(); // save the first that will be reversed
        next.ActiveList.pop_back();
        uint8_t newPhysReg = next.RegisterMapTable[entry.LogicalDestination]; // takes the ph register mapped to this instr destination
        next.RegisterMapTable[entry.LogicalDestination] = entry.OldDestination; // restore the old mapping in the register map table
        next.FreeList.push_back(newPhysReg); // we free the ph register that was allocated for this instr 
        next.BusyBitTable[newPhysReg] = false; // we also clear it's busy bit
    }
    // the clearing of the exception flag in next cycle
    return;
    }

    // here instead the case of the normal commit (without exception)
    // Retire up to 4 instructions from the front of the Active List (oldest first to keep the architectural order)

    int commitCount = 0;

    while (commitCount < 4 && !next.ActiveList.empty()) { // as long as list not empty and we have not committed 4 instructions yet
        ActiveListStruct front = next.ActiveList.front(); // each time we take the front instr

        if (!front.Done) { // we get to an instruction that has not finished yet
            break; // instruction not completed yet so even the younger ones can't be committed
        }

        if (front.Exception) { // if we get to an instruction that caused an exception
            next.ExceptionFlag = true; // we set the ExceptionFlag to true
            // setting the flag is the indirect way to notify the F&D stage of the exception
            next.ExceptionPC = front.PC; // save the pc of the ones caused exception
            // reset the integer queue and the execution stage
            next.IntegerQueue.clear();
            next.ExecutingInstructions.clear(); 

            break;
        }

        // case normal so no exception
        next.FreeList.push_back(front.OldDestination); // the old physical register is no longer needed so return to free list
        next.ActiveList.pop_front(); // remove the instruction from the active list
        commitCount++; // increase the count of committed instructions
    }
}

void propagate(ProcessorStateStruct& state, const std::vector<DecodedInstructionStruct>& program) {

    ProcessorStateStruct next = state; // make a copy of the current state to modify for the next cycle
    next.BackpressureFlag = false; // maybe can be removed (to check)

    // All stages run in reverse pipeline order
    commit(state, next);
    execute(state, next);
    issue(state, next);
    renameDispatch(state, next);
    fetchDecode(state, next, program);

    // Latch, replace current with next
    state = next;
}