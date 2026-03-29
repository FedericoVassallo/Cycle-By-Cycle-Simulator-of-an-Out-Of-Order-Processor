#!/usr/bin/env python3
"""
Independent cycle-accurate reference simulator for the OoO470 processor.
Written directly from the homework specification (not from the C++ code).

Usage:
    python3 ref_sim.py input.json output.json
"""

import json
import sys
import copy
from collections import deque


def parse_instruction(line, pc):
    """Parse one instruction string into a dict."""
    clean = line.replace(",", " ").split()
    opcode_str = clean[0]
    rd_str = clean[1]
    rs1_str = clean[2]
    operand_str = clean[3]

    rd = int(rd_str[1:])   # strip 'x'
    rs1 = int(rs1_str[1:])

    if opcode_str == "addi":
        return {
            "OpCode": "addi", "PC": pc,
            "rd": rd, "rs1": rs1, "rs2": 0,
            "imm": int(operand_str), "hasImm": True
        }
    else:
        rs2 = int(operand_str[1:])
        return {
            "OpCode": opcode_str, "PC": pc,
            "rd": rd, "rs1": rs1, "rs2": rs2,
            "imm": 0, "hasImm": False
        }


def make_initial_state():
    """Create the reset state of the processor."""
    return {
        "PC": 0,
        "PhysicalRegisterFile": [0] * 64,
        "ExceptionPC": 0,
        "ExceptionFlag": False,
        "RegisterMapTable": list(range(32)),
        "BackpressureFlag": False,
        "BusyBitTable": [False] * 64,
        "FreeList": deque(range(32, 64)),
        "ActiveList": deque(),       # deque of dicts
        "IntegerQueue": [],          # list of dicts
        "DecodedInstructionRegister": [],  # list of instruction dicts
        "ExecutingInstructions": [],  # list of dicts
    }


def snapshot_state(state):
    """Serialize state to JSON-compatible dict (matching spec format)."""
    snap = {}
    snap["PC"] = state["PC"]
    snap["ExceptionPC"] = state["ExceptionPC"]
    snap["Exception"] = state["ExceptionFlag"]
    snap["PhysicalRegisterFile"] = list(state["PhysicalRegisterFile"])
    snap["RegisterMapTable"] = list(state["RegisterMapTable"])
    snap["BusyBitTable"] = list(state["BusyBitTable"])
    snap["FreeList"] = list(state["FreeList"])
    snap["DecodedPCs"] = [instr["PC"] for instr in state["DecodedInstructionRegister"]]

    snap["ActiveList"] = []
    for e in state["ActiveList"]:
        snap["ActiveList"].append({
            "Done": e["Done"],
            "Exception": e["Exception"],
            "LogicalDestination": e["LogicalDestination"],
            "OldDestination": e["OldDestination"],
            "PC": e["PC"],
        })

    snap["IntegerQueue"] = []
    for e in state["IntegerQueue"]:
        opcode_out = e["OpCode"]
        if opcode_out == "addi":
            opcode_out = "add"
        snap["IntegerQueue"].append({
            "DestRegister": e["DestRegister"],
            "OpAIsReady": e["OpAIsReady"],
            "OpARegTag": e["OpARegTag"],
            "OpAValue": e["OpAValue"],
            "OpBIsReady": e["OpBIsReady"],
            "OpBRegTag": e["OpBRegTag"],
            "OpBValue": e["OpBValue"],
            "OpCode": opcode_out,
            "PC": e["PC"],
        })

    return snap


def deep_copy_state(state):
    """Deep copy the processor state for next-cycle computation."""
    nxt = {
        "PC": state["PC"],
        "PhysicalRegisterFile": list(state["PhysicalRegisterFile"]),
        "ExceptionPC": state["ExceptionPC"],
        "ExceptionFlag": state["ExceptionFlag"],
        "RegisterMapTable": list(state["RegisterMapTable"]),
        "BackpressureFlag": state["BackpressureFlag"],
        "BusyBitTable": list(state["BusyBitTable"]),
        "FreeList": deque(state["FreeList"]),
        "ActiveList": deque(),
        "IntegerQueue": [],
        "DecodedInstructionRegister": [],
        "ExecutingInstructions": [],
    }
    for e in state["ActiveList"]:
        nxt["ActiveList"].append(dict(e))
    for e in state["IntegerQueue"]:
        nxt["IntegerQueue"].append(dict(e))
    for e in state["DecodedInstructionRegister"]:
        nxt["DecodedInstructionRegister"].append(dict(e))
    for e in state["ExecutingInstructions"]:
        nxt["ExecutingInstructions"].append(dict(e))
    return nxt


# ─── Pipeline stages ────────────────────────────────────────────────

def commit_stage(current, nxt):
    """Commit stage: retire or rollback."""
    if current["ExceptionFlag"]:
        # Exception mode: rollback
        if len(nxt["ActiveList"]) == 0:
            nxt["ExceptionFlag"] = False
            return
        rollback_count = min(4, len(nxt["ActiveList"]))
        for _ in range(rollback_count):
            entry = nxt["ActiveList"].pop()  # youngest first (back)
            allocated_preg = nxt["RegisterMapTable"][entry["LogicalDestination"]]
            nxt["RegisterMapTable"][entry["LogicalDestination"]] = entry["OldDestination"]
            nxt["FreeList"].append(allocated_preg)
            nxt["BusyBitTable"][allocated_preg] = False
        return

    # Normal commit
    committed = 0
    while committed < 4 and len(nxt["ActiveList"]) > 0:
        front = nxt["ActiveList"][0]
        if not front["Done"]:
            break
        if front["Exception"]:
            nxt["ExceptionFlag"] = True
            nxt["ExceptionPC"] = front["PC"]
            nxt["IntegerQueue"].clear()
            nxt["ExecutingInstructions"].clear()
            break
        # Normal retire
        nxt["FreeList"].append(front["OldDestination"])
        nxt["ActiveList"].popleft()
        committed += 1


def execute_stage(current, nxt):
    """Execute stage: tick ALUs, compute results, forward.
    Reads from CURRENT ExecutingInstructions (pipeline register, not same-cycle queue).
    Even if commit cleared nxt ExecutingInstructions due to exception, ALU results
    are still produced this cycle."""
    MASK64 = (1 << 64) - 1

    # Clear nxt executing — we rebuild from current
    nxt["ExecutingInstructions"] = []

    for ex in current["ExecutingInstructions"]:
        ex = dict(ex)  # work on a copy
        ex["CyclesLeft"] -= 1

        if ex["CyclesLeft"] == 0:
            # Compute result
            opA = ex["OpAValue"]
            opB = ex["OpBValue"]
            opcode = ex["OpCode"]
            exception = False

            if opcode in ("add", "addi"):
                result = (opA + opB) & MASK64
            elif opcode == "sub":
                result = (opA - opB) & MASK64
            elif opcode == "mulu":
                result = (opA * opB) & MASK64
            elif opcode == "divu":
                if opB == 0:
                    exception = True
                    result = 0
                else:
                    result = opA // opB
            elif opcode == "remu":
                if opB == 0:
                    exception = True
                    result = 0
                else:
                    result = opA % opB
            else:
                raise ValueError(f"Unknown opcode: {opcode}")

            ex["Result"] = result
            ex["ExceptionCaused"] = exception

            # Mark done in ActiveList
            for al_entry in nxt["ActiveList"]:
                if al_entry["PC"] == ex["PC"] and not al_entry["Done"]:
                    al_entry["Done"] = True
                    al_entry["Exception"] = exception
                    break

            # Write result and forward (only if no exception from this instruction)
            if not exception:
                nxt["PhysicalRegisterFile"][ex["rd"]] = result
                nxt["BusyBitTable"][ex["rd"]] = False

                for iq_entry in nxt["IntegerQueue"]:
                    if not iq_entry["OpAIsReady"] and iq_entry["OpARegTag"] == ex["rd"]:
                        iq_entry["OpAIsReady"] = True
                        iq_entry["OpARegTag"] = 0
                        iq_entry["OpAValue"] = result
                    if not iq_entry["OpBIsReady"] and iq_entry["OpBRegTag"] == ex["rd"]:
                        iq_entry["OpBIsReady"] = True
                        iq_entry["OpBRegTag"] = 0
                        iq_entry["OpBValue"] = result
            # Completed — do NOT add back
        else:
            # Not finished — keep for next cycle, unless exception flushed pipeline
            if not nxt["ExceptionFlag"]:
                nxt["ExecutingInstructions"].append(ex)


def issue_stage(current, nxt):
    """Issue stage: pick up to 4 oldest ready instructions."""
    issued = 0
    i = 0
    while i < len(nxt["IntegerQueue"]) and issued < 4:
        iq = nxt["IntegerQueue"][i]
        if iq["OpAIsReady"] and iq["OpBIsReady"]:
            ex_instr = {
                "rd": iq["DestRegister"],
                "OpCode": iq["OpCode"],
                "OpAValue": iq["OpAValue"],
                "OpBValue": iq["OpBValue"],
                "ExceptionCaused": False,
                "Result": 0,
                "PC": iq["PC"],
                "CyclesLeft": 2,
            }
            nxt["ExecutingInstructions"].append(ex_instr)
            nxt["IntegerQueue"].pop(i)
            issued += 1
            # don't increment i since we removed an element
        else:
            i += 1


def rename_dispatch_stage(current, nxt):
    """Rename and Dispatch stage."""
    if nxt["ExceptionFlag"]:
        return

    dir_size = len(nxt["DecodedInstructionRegister"])
    if dir_size == 0:
        return

    # Check resources: FreeList, ActiveList (max 32), IntegerQueue (max 32)
    if (len(nxt["FreeList"]) < dir_size or
        len(nxt["ActiveList"]) + dir_size > 32 or
        len(nxt["IntegerQueue"]) + dir_size > 32):
        nxt["BackpressureFlag"] = True
        return

    # Process all instructions atomically
    for instr in nxt["DecodedInstructionRegister"]:
        # Allocate physical register
        new_phys = nxt["FreeList"].popleft()

        # Read source physical registers from current map
        phys_rs1 = nxt["RegisterMapTable"][instr["rs1"]]
        phys_rs2 = nxt["RegisterMapTable"][instr["rs2"]]

        # Create ActiveList entry
        al_entry = {
            "Done": False,
            "Exception": False,
            "LogicalDestination": instr["rd"],
            "OldDestination": nxt["RegisterMapTable"][instr["rd"]],
            "PC": instr["PC"],
        }
        nxt["ActiveList"].append(al_entry)

        # Update RegisterMapTable and BusyBitTable
        nxt["RegisterMapTable"][instr["rd"]] = new_phys
        nxt["BusyBitTable"][new_phys] = True

        # Create IntegerQueue entry
        iq_entry = {
            "DestRegister": new_phys,
            "OpCode": instr["OpCode"],
            "PC": instr["PC"],
            "OpAIsReady": False,
            "OpARegTag": 0,
            "OpAValue": 0,
            "OpBIsReady": False,
            "OpBRegTag": 0,
            "OpBValue": 0,
        }

        # Operand A
        if not nxt["BusyBitTable"][phys_rs1]:
            iq_entry["OpAIsReady"] = True
            iq_entry["OpARegTag"] = 0
            iq_entry["OpAValue"] = nxt["PhysicalRegisterFile"][phys_rs1]
        else:
            iq_entry["OpAIsReady"] = False
            iq_entry["OpARegTag"] = phys_rs1
            iq_entry["OpAValue"] = 0

        # Operand B
        if instr["hasImm"]:
            # Convert signed immediate to unsigned 64-bit
            imm = instr["imm"]
            if imm < 0:
                imm = imm + (1 << 64)
            iq_entry["OpBIsReady"] = True
            iq_entry["OpBRegTag"] = 0
            iq_entry["OpBValue"] = imm
        else:
            if not nxt["BusyBitTable"][phys_rs2]:
                iq_entry["OpBIsReady"] = True
                iq_entry["OpBRegTag"] = 0
                iq_entry["OpBValue"] = nxt["PhysicalRegisterFile"][phys_rs2]
            else:
                iq_entry["OpBIsReady"] = False
                iq_entry["OpBRegTag"] = phys_rs2
                iq_entry["OpBValue"] = 0

        nxt["IntegerQueue"].append(iq_entry)

    nxt["DecodedInstructionRegister"].clear()
    nxt["BackpressureFlag"] = False


def fetch_decode_stage(current, nxt, program):
    """Fetch and Decode stage."""
    if nxt["ExceptionFlag"]:
        nxt["PC"] = 0x10000
        nxt["DecodedInstructionRegister"].clear()
        return

    if nxt["BackpressureFlag"]:
        return

    if nxt["PC"] >= len(program):
        return

    start_pc = nxt["PC"]
    count = min(4, len(program) - start_pc)

    for i in range(count):
        nxt["DecodedInstructionRegister"].append(dict(program[start_pc + i]))

    nxt["PC"] = start_pc + count


def propagate(current_state, program):
    """Run one cycle: all stages in reverse pipeline order on a copy."""
    nxt = deep_copy_state(current_state)
    nxt["BackpressureFlag"] = False

    commit_stage(current_state, nxt)
    execute_stage(current_state, nxt)
    issue_stage(current_state, nxt)
    rename_dispatch_stage(current_state, nxt)
    fetch_decode_stage(current_state, nxt, program)

    return nxt


def simulate(program):
    """Run the full simulation, return list of snapshots."""
    state = make_initial_state()
    log = [snapshot_state(state)]

    while (state["PC"] < len(program) or
           len(state["DecodedInstructionRegister"]) > 0 or
           len(state["ActiveList"]) > 0 or
           state["ExceptionFlag"]):
        state = propagate(state, program)
        log.append(snapshot_state(state))

    return log


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.json> <output.json>", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1]) as f:
        raw = json.load(f)

    program = [parse_instruction(line, i) for i, line in enumerate(raw)]
    log = simulate(program)

    with open(sys.argv[2], "w") as f:
        json.dump(log, f, indent=4)


if __name__ == "__main__":
    main()
