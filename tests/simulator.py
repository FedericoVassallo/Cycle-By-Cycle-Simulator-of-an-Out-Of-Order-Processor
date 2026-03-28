#!/usr/bin/env python3
"""
Cycle-accurate simulator for OoO470 processor (MIPS R10000-like).

Key design decisions from reference analysis:
1. Stages in reverse order: Commit → Forwarding/Execute → Issue → R&D → F&D
2. Commit uses pre-forwarding Done flags (forwarding marks Done AFTER commit reads it)
3. Excepting instructions on forwarding: mark AL Done+Exception but do NOT update PRF/BBT/IQ
4. Exception entry: do NOT remove excepting instruction from AL, do NOT rollback immediately
5. Exception recovery: roll back 4 per cycle from tail, exit when AL empty
6. When operand is ready, RegTag is set to 0 (don't-care)
"""

import json
import sys

UINT64_MAX = (1 << 64) - 1

def parse_instruction(s, pc):
    s = s.strip()
    parts = s.split()
    opcode = parts[0]
    args = ''.join(parts[1:]).split(',')
    def parse_reg(r):
        return int(r.strip()[1:])
    instr = {'pc': pc, 'opcode': opcode, 'dest': parse_reg(args[0]), 'opA': parse_reg(args[1])}
    if opcode == 'addi':
        instr['imm'] = int(args[2])
        instr['is_imm'] = True
    else:
        instr['opB'] = parse_reg(args[2])
        instr['is_imm'] = False
    return instr

def sign_extend_64(imm):
    return imm & UINT64_MAX

def execute_op(opcode, val_a, val_b):
    if opcode == 'add':
        return ((val_a + val_b) & UINT64_MAX, False)
    elif opcode == 'sub':
        return ((val_a - val_b) & UINT64_MAX, False)
    elif opcode == 'mulu':
        return ((val_a * val_b) & UINT64_MAX, False)
    elif opcode == 'divu':
        if val_b == 0:
            return (0, True)
        return (val_a // val_b, False)
    elif opcode == 'remu':
        if val_b == 0:
            return (0, True)
        return (val_a % val_b, False)
    raise ValueError(f"Unknown opcode: {opcode}")


class OoO470Simulator:
    def __init__(self, program):
        self.program = program
        self.log = []
        self.pc = 0
        self.prf = [0] * 64
        self.decoded_instrs = []
        self.exception_flag = False
        self.exception_pc = 0
        self.rmt = list(range(32))
        self.free_list = list(range(32, 64))
        self.bbt = [False] * 64
        self.active_list = []
        self.integer_queue = []
        self.alu_stage1 = [None] * 4
        self.alu_stage2 = [None] * 4

    def dump_state(self):
        state = {
            "PC": self.pc,
            "PhysicalRegisterFile": list(self.prf),
            "DecodedPCs": [instr['pc'] for instr in self.decoded_instrs],
            "Exception": self.exception_flag,
            "ExceptionPC": self.exception_pc,
            "RegisterMapTable": list(self.rmt),
            "FreeList": list(self.free_list),
            "BusyBitTable": list(self.bbt),
            "ActiveList": [
                {"Done": e['done'], "Exception": e['exception'],
                 "LogicalDestination": e['logical_dest'],
                 "OldDestination": e['old_dest'], "PC": e['pc']}
                for e in self.active_list
            ],
            "IntegerQueue": [
                {"DestRegister": e['dest_reg'],
                 "OpAIsReady": e['opA_ready'], "OpARegTag": e['opA_tag'], "OpAValue": e['opA_value'],
                 "OpBIsReady": e['opB_ready'], "OpBRegTag": e['opB_tag'], "OpBValue": e['opB_value'],
                 "OpCode": e['opcode'], "PC": e['pc']}
                for e in self.integer_queue
            ],
        }
        self.log.append(state)

    def is_finished(self):
        all_fetched = (self.pc >= len(self.program) or self.pc >= 0x10000) and len(self.decoded_instrs) == 0
        return (all_fetched and len(self.active_list) == 0 and
                all(s is None for s in self.alu_stage1) and
                all(s is None for s in self.alu_stage2) and
                len(self.integer_queue) == 0 and not self.exception_flag)

    def run(self):
        self.dump_state()
        while not self.is_finished():
            self.propagate()
            self.dump_state()
        return self.log

    def propagate(self):
        # === Compute forwarding results from ALU stage 2 ===
        forwarding_ok = {}    # preg -> (value, pc) for non-excepting results
        forwarding_exc = {}   # preg -> pc for excepting results
        for i in range(4):
            if self.alu_stage2[i] is not None:
                e = self.alu_stage2[i]
                result, is_exc = execute_op(e['opcode'], e['opA_value'], e['opB_value'])
                if is_exc:
                    forwarding_exc[e['dest_reg']] = (result, e['pc'])
                else:
                    forwarding_ok[e['dest_reg']] = (result, e['pc'])

        # === COMMIT STAGE (reads pre-forwarding AL Done flags) ===
        committed_old_dests = []
        entering_exception = False

        if self.exception_flag:
            # Exception recovery: check if AL is already empty first
            if len(self.active_list) == 0:
                self.exception_flag = False
            else:
                # Roll back up to 4 from tail
                for _ in range(min(4, len(self.active_list))):
                    entry = self.active_list.pop()
                    self.rmt[entry['logical_dest']] = entry['old_dest']
                    self.free_list.append(entry['new_dest'])
                    self.bbt[entry['new_dest']] = False
        else:
            # Normal commit
            commit_count = 0
            while commit_count < 4 and len(self.active_list) > 0:
                head = self.active_list[0]
                if not head['done']:
                    break
                if head['exception']:
                    # Exception detected: enter exception mode
                    # Do NOT remove the excepting instruction from AL
                    # Do NOT free its old destination
                    entering_exception = True
                    break
                else:
                    # Normal retire
                    self.active_list.pop(0)
                    committed_old_dests.append(head['old_dest'])
                    commit_count += 1

        # Free old destination registers from normally committed instructions
        for reg in committed_old_dests:
            self.free_list.append(reg)

        # === FORWARDING ===
        # Non-excepting results: update PRF, BBT, AL Done, IQ operands
        for preg, (val, fwd_pc) in forwarding_ok.items():
            self.prf[preg] = val
            self.bbt[preg] = False

        # Excepting results: only mark AL Done+Exception, do NOT update PRF/BBT/IQ
        # (handled below with AL marking)

        # === Handle entering exception mode ===
        if entering_exception:
            self.exception_flag = True
            self.exception_pc = self.active_list[0]['pc']  # head is the excepting instr
            self.pc = 0x10000
            self.decoded_instrs = []
            self.integer_queue = []
            self.alu_stage1 = [None] * 4
            self.alu_stage2 = [None] * 4
            # Mark AL entries Done from forwarding (still needed for state consistency)
            for preg, (val, fwd_pc) in forwarding_ok.items():
                for al_entry in self.active_list:
                    if al_entry['new_dest'] == preg and al_entry['pc'] == fwd_pc:
                        al_entry['done'] = True
                        break
            for preg, (val, fwd_pc) in forwarding_exc.items():
                for al_entry in self.active_list:
                    if al_entry['new_dest'] == preg and al_entry['pc'] == fwd_pc:
                        al_entry['done'] = True
                        al_entry['exception'] = True
                        break
            # Do NOT rollback in the entering cycle
            return

        if self.exception_flag:
            # During exception recovery, only forwarding PRF/BBT updates already done above
            # No fetch, no issue, no dispatch
            # Advance ALU pipeline (should be empty)
            for i in range(4):
                self.alu_stage2[i] = self.alu_stage1[i]
                self.alu_stage1[i] = None
            return

        # Mark AL entries Done from non-excepting forwarding
        for preg, (val, fwd_pc) in forwarding_ok.items():
            for al_entry in self.active_list:
                if al_entry['new_dest'] == preg and al_entry['pc'] == fwd_pc:
                    al_entry['done'] = True
                    break

        # Mark AL entries Done+Exception from excepting forwarding
        for preg, (val, fwd_pc) in forwarding_exc.items():
            for al_entry in self.active_list:
                if al_entry['new_dest'] == preg and al_entry['pc'] == fwd_pc:
                    al_entry['done'] = True
                    al_entry['exception'] = True
                    break

        # Update IQ entries with non-excepting forwarding ONLY
        for iq_entry in self.integer_queue:
            if not iq_entry['opA_ready'] and iq_entry['opA_tag'] in forwarding_ok:
                iq_entry['opA_ready'] = True
                iq_entry['opA_value'] = forwarding_ok[iq_entry['opA_tag']][0]
                iq_entry['opA_tag'] = 0
            if not iq_entry['opB_ready'] and iq_entry['opB_tag'] in forwarding_ok:
                iq_entry['opB_ready'] = True
                iq_entry['opB_value'] = forwarding_ok[iq_entry['opB_tag']][0]
                iq_entry['opB_tag'] = 0

        # === EXECUTE: advance ALU pipeline ===
        new_alu_stage2 = list(self.alu_stage1)
        new_alu_stage1 = [None] * 4

        # === ISSUE ===
        ready = [e for e in self.integer_queue if e['opA_ready'] and e['opB_ready']]
        ready.sort(key=lambda e: e['pc'])

        issued_ids = set()
        alu_idx = 0
        for entry in ready:
            if alu_idx >= 4:
                break
            new_alu_stage1[alu_idx] = entry
            issued_ids.add(id(entry))
            alu_idx += 1

        self.integer_queue = [e for e in self.integer_queue if id(e) not in issued_ids]
        self.alu_stage2 = new_alu_stage2
        self.alu_stage1 = new_alu_stage1

        # === RENAME & DISPATCH ===
        # Combine both forwarding dicts for operand readiness check during dispatch
        # But only non-excepting forwarding provides values for operands
        backpressure = False
        if len(self.decoded_instrs) > 0:
            n = len(self.decoded_instrs)
            if (len(self.free_list) >= n and
                (32 - len(self.active_list)) >= n and
                (32 - len(self.integer_queue)) >= n):

                for instr in self.decoded_instrs:
                    new_phys = self.free_list.pop(0)
                    old_phys = self.rmt[instr['dest']]

                    # Read operand A
                    opA_phys = self.rmt[instr['opA']]
                    if not self.bbt[opA_phys]:
                        opA_ready, opA_value, opA_tag = True, self.prf[opA_phys], 0
                    elif opA_phys in forwarding_ok:
                        opA_ready, opA_value, opA_tag = True, forwarding_ok[opA_phys][0], 0
                    else:
                        opA_ready, opA_value, opA_tag = False, 0, opA_phys

                    # Read operand B
                    if instr['is_imm']:
                        opB_ready, opB_value, opB_tag = True, sign_extend_64(instr['imm']), 0
                    else:
                        opB_phys = self.rmt[instr['opB']]
                        if not self.bbt[opB_phys]:
                            opB_ready, opB_value, opB_tag = True, self.prf[opB_phys], 0
                        elif opB_phys in forwarding_ok:
                            opB_ready, opB_value, opB_tag = True, forwarding_ok[opB_phys][0], 0
                        else:
                            opB_ready, opB_value, opB_tag = False, 0, opB_phys

                    self.rmt[instr['dest']] = new_phys
                    self.bbt[new_phys] = True

                    iq_opcode = 'add' if instr['opcode'] == 'addi' else instr['opcode']

                    self.active_list.append({
                        'done': False, 'exception': False,
                        'logical_dest': instr['dest'], 'old_dest': old_phys,
                        'new_dest': new_phys, 'pc': instr['pc'],
                    })
                    self.integer_queue.append({
                        'dest_reg': new_phys,
                        'opA_ready': opA_ready, 'opA_tag': opA_tag, 'opA_value': opA_value,
                        'opB_ready': opB_ready, 'opB_tag': opB_tag, 'opB_value': opB_value,
                        'opcode': iq_opcode, 'pc': instr['pc'],
                    })

                self.decoded_instrs = []
            else:
                backpressure = True

        # === FETCH & DECODE ===
        if not backpressure and len(self.decoded_instrs) == 0 and self.pc < len(self.program):
            fetch_count = min(4, len(self.program) - self.pc)
            if fetch_count > 0:
                self.decoded_instrs = [self.program[self.pc + i] for i in range(fetch_count)]
                self.pc += fetch_count

    def save(self, filename):
        with open(filename, 'w') as f:
            json.dump(self.log, f, indent=4)


def main():
    if len(sys.argv) < 3:
        print("Usage: python3 simulator.py input.json output.json")
        sys.exit(1)
    with open(sys.argv[1]) as f:
        raw = json.load(f)
    program = [parse_instruction(s, i) for i, s in enumerate(raw)]
    sim = OoO470Simulator(program)
    sim.run()
    sim.save(sys.argv[2])


if __name__ == '__main__':
    main()
