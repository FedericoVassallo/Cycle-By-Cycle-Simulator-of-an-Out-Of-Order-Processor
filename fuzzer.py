#!/usr/bin/env python3
"""
Fuzzer for OoO470 processor simulator.
Generates random valid programs and optionally runs them against
your simulator and the reference to find mismatches.

Usage:
  # Generate 100 random test programs in tests_fuzz/
  python3 fuzzer.py --generate 100

  # Generate and run against both simulators, report mismatches
  python3 fuzzer.py --generate 100 --run --sim ./simulator --ref ./ref_simulator

  # Just generate with specific seed for reproducibility
  python3 fuzzer.py --generate 50 --seed 42
"""

import json
import random
import os
import argparse
import subprocess
import sys

OPCODES_REG = ["add", "sub", "mulu", "divu", "remu"]  # register-register
OPCODES_IMM = ["addi"]  # register-immediate
ALL_OPCODES = OPCODES_REG + OPCODES_IMM

def gen_register():
    """Random register x0-x31"""
    return f"x{random.randint(0, 31)}"

def gen_immediate():
    """Random immediate value, biased toward interesting values"""
    choice = random.random()
    if choice < 0.15:
        return 0  # zero — triggers divide-by-zero if loaded then used as divisor
    elif choice < 0.30:
        return random.choice([1, -1, 2, -2])  # small values
    elif choice < 0.50:
        return random.randint(-100, 100)  # moderate range
    elif choice < 0.70:
        return random.randint(-2**15, 2**15 - 1)  # 16-bit range
    elif choice < 0.85:
        return random.randint(-2**31, 2**31 - 1)  # 32-bit range
    else:
        return random.randint(-2**62, 2**62 - 1)  # large values

def gen_instruction(profile="balanced"):
    """Generate a single random instruction string."""
    if profile == "heavy_exception":
        # Bias toward divu/remu and registers likely to be zero
        opcode = random.choice(["divu", "remu", "add", "addi", "sub"])
    elif profile == "no_exception":
        opcode = random.choice(["add", "addi", "sub", "mulu"])
    elif profile == "addi_only":
        opcode = "addi"
    elif profile == "dependency_chain":
        opcode = random.choice(["add", "addi", "sub"])
    else:
        opcode = random.choice(ALL_OPCODES)

    rd = gen_register()
    rs1 = gen_register()

    if opcode == "addi":
        imm = gen_immediate()
        return f"{opcode} {rd}, {rs1}, {imm}"
    else:
        rs2 = gen_register()
        return f"{opcode} {rd}, {rs1}, {rs2}"


def gen_dependency_chain(length):
    """Generate a chain where each instruction depends on the previous one."""
    instrs = []
    # Start with a value in some register
    reg = random.randint(1, 31)
    instrs.append(f"addi x{reg}, x0, {random.randint(1, 50)}")

    for _ in range(length - 1):
        opcode = random.choice(["add", "addi", "sub"])
        src = reg
        if opcode == "addi":
            reg = random.randint(1, 31)
            imm = random.randint(-10, 10)
            instrs.append(f"addi x{reg}, x{src}, {imm}")
        else:
            other_src = random.randint(0, 31)
            reg = random.randint(1, 31)
            instrs.append(f"{opcode} x{reg}, x{src}, x{other_src}")

    return instrs


def gen_program(min_len=1, max_len=50, profile="balanced"):
    """Generate a complete random program."""
    length = random.randint(min_len, max_len)

    if profile == "dependency_chain":
        return gen_dependency_chain(length)

    if profile == "backpressure":
        # Many instructions with dependencies to fill up structures
        instrs = []
        # First load values
        for i in range(min(8, length)):
            instrs.append(f"addi x{i+1}, x0, {random.randint(1, 100)}")
        # Then create dependent chains that will clog the pipeline
        for i in range(length - len(instrs)):
            r1 = random.randint(1, 8)
            r2 = random.randint(1, 8)
            rd = random.randint(1, 31)
            opcode = random.choice(["add", "sub", "mulu"])
            instrs.append(f"{opcode} x{rd}, x{r1}, x{r2}")
        return instrs

    if profile == "exception_at_end":
        instrs = [gen_instruction("no_exception") for _ in range(length - 1)]
        # Force a divide by zero at the end
        instrs.append(f"divu x{random.randint(1,31)}, x{random.randint(1,31)}, x0")
        return instrs

    if profile == "exception_early":
        # Put exception very early, many instructions after
        pos = random.randint(0, min(3, length - 1))
        instrs = [gen_instruction("no_exception") for _ in range(pos)]
        instrs.append(f"divu x{random.randint(1,31)}, x{random.randint(1,31)}, x0")
        for _ in range(length - len(instrs)):
            instrs.append(gen_instruction("no_exception"))
        return instrs

    if profile == "exception_middle":
        mid = length // 2
        instrs = [gen_instruction("no_exception") for _ in range(mid)]
        instrs.append(f"divu x{random.randint(1,31)}, x{random.randint(1,31)}, x0")
        for _ in range(length - len(instrs)):
            instrs.append(gen_instruction("no_exception"))
        return instrs

    if profile == "exception_dep_after":
        # KEY PATTERN from professor's tests:
        # The excepting instruction writes to register R, and instructions
        # AFTER it read from R. Tests that dependent-on-exception instructions
        # are properly rolled back.
        exc_reg = random.randint(0, 31)
        instrs = []
        # Some setup instructions
        setup_count = random.randint(4, max(4, min(16, length // 2)))
        for _ in range(setup_count):
            instrs.append(gen_instruction("no_exception"))
        # The excepting instruction writes to exc_reg
        exc_op = random.choice(["divu", "remu"])
        # Divide by a register that is likely zero (x0 is 0 at init, or use a known-zero)
        instrs.append(f"{exc_op} x{exc_reg}, x{random.randint(0,31)}, x0")
        # Instructions AFTER the exception that READ from exc_reg
        for _ in range(length - len(instrs)):
            opcode = random.choice(["add", "addi", "sub"])
            rd = random.randint(0, 31)
            if opcode == "addi":
                # Use exc_reg as source
                if random.random() < 0.7:
                    instrs.append(f"addi x{rd}, x{exc_reg}, {random.randint(-100, 100)}")
                else:
                    instrs.append(gen_instruction("no_exception"))
            else:
                # Use exc_reg as one of the sources
                other = random.randint(0, 31)
                if random.random() < 0.5:
                    instrs.append(f"{opcode} x{rd}, x{exc_reg}, x{other}")
                else:
                    instrs.append(f"{opcode} x{rd}, x{other}, x{exc_reg}")
        return instrs

    if profile == "exception_chain":
        # Exception instruction's dest is used by next, which is used by next, etc.
        # Deep dependency chain rooted in the excepting instruction
        exc_reg = random.randint(1, 31)
        instrs = []
        setup_count = random.randint(2, max(2, min(8, length // 3)))
        for _ in range(setup_count):
            instrs.append(gen_instruction("no_exception"))
        exc_op = random.choice(["divu", "remu"])
        instrs.append(f"{exc_op} x{exc_reg}, x{random.randint(0,31)}, x0")
        # Chain: each reads from previous dest
        cur_reg = exc_reg
        for _ in range(length - len(instrs)):
            next_reg = random.randint(1, 31)
            opcode = random.choice(["add", "addi", "sub"])
            if opcode == "addi":
                instrs.append(f"addi x{next_reg}, x{cur_reg}, {random.randint(-10, 10)}")
            else:
                other = random.randint(0, 31)
                instrs.append(f"{opcode} x{next_reg}, x{cur_reg}, x{other}")
            cur_reg = next_reg
        return instrs

    if profile == "multi_exception":
        # Multiple divu/remu instructions, some may exception, some may not
        # Tests that only the FIRST exception in program order matters
        instrs = []
        for _ in range(length):
            if random.random() < 0.3:
                exc_op = random.choice(["divu", "remu"])
                rd = gen_register()
                rs1 = gen_register()
                # Sometimes divide by x0 (likely zero), sometimes by other regs
                if random.random() < 0.5:
                    instrs.append(f"{exc_op} {rd}, {rs1}, x0")
                else:
                    instrs.append(f"{exc_op} {rd}, {rs1}, {gen_register()}")
            else:
                instrs.append(gen_instruction("no_exception"))
        return instrs

    if profile == "professor_style":
        # Mimics the structure of the professor's test:
        # Blocks of 4 instructions with different patterns,
        # exception in the middle, deps crossing blocks
        instrs = []
        n_blocks = max(2, length // 4)
        exc_block = random.randint(1, n_blocks - 1)
        exc_reg = random.randint(0, 31)
        for b in range(n_blocks):
            if b == exc_block:
                # Exception block
                exc_op = random.choice(["divu", "remu"])
                instrs.append(f"{exc_op} x{exc_reg}, x{random.randint(0,31)}, x0")
                # Rest of block reads from exc_reg
                for _ in range(3):
                    rd = random.randint(0, 31)
                    instrs.append(f"addi x{rd}, x{exc_reg}, {random.randint(1, 100)}")
            elif b < exc_block:
                # Pre-exception: setup values
                for _ in range(4):
                    instrs.append(gen_instruction("no_exception"))
            else:
                # Post-exception: use registers from before and after exception
                for _ in range(4):
                    opcode = random.choice(["add", "sub", "addi"])
                    rd = random.randint(0, 31)
                    if opcode == "addi":
                        instrs.append(f"addi x{rd}, x{random.randint(0,31)}, {random.randint(-50,50)}")
                    else:
                        instrs.append(f"{opcode} x{rd}, x{random.randint(0,31)}, x{random.randint(0,31)}")
        return instrs[:length]  # trim to requested length

    if profile == "same_dest":
        # Many instructions writing to the same register (WAW hazards)
        dest = random.randint(1, 31)
        instrs = []
        for _ in range(length):
            opcode = random.choice(ALL_OPCODES)
            rs1 = gen_register()
            if opcode == "addi":
                instrs.append(f"addi x{dest}, {rs1}, {gen_immediate()}")
            else:
                rs2 = gen_register()
                instrs.append(f"{opcode} x{dest}, {rs1}, {rs2}")
        return instrs

    if profile == "x0_dest":
        # Write to x0 (should still work, x0 is not special in this ISA)
        instrs = []
        for _ in range(length):
            opcode = random.choice(ALL_OPCODES)
            rs1 = gen_register()
            if opcode == "addi":
                instrs.append(f"addi x0, {rs1}, {gen_immediate()}")
            else:
                rs2 = gen_register()
                instrs.append(f"{opcode} x0, {rs1}, {rs2}")
        return instrs

    # Default balanced
    return [gen_instruction(profile) for _ in range(length)]


def generate_tests(output_dir, count, seed=None):
    """Generate count random test programs."""
    if seed is not None:
        random.seed(seed)

    os.makedirs(output_dir, exist_ok=True)

    profiles = [
        "balanced",
        "no_exception",
        "heavy_exception",
        "dependency_chain",
        "backpressure",
        "exception_at_end",
        "exception_early",
        "exception_middle",
        "exception_dep_after",   # exception dest read by later instructions
        "exception_chain",       # deep dep chain rooted in excepting instr
        "multi_exception",       # multiple divu/remu scattered
        "professor_style",       # blocks of 4, exception mid-program
        "same_dest",
        "addi_only",
        "x0_dest",
    ]

    lengths = [
        (1, 1),      # single instruction
        (2, 4),      # tiny programs
        (5, 10),     # small
        (11, 20),    # medium
        (21, 40),    # large
        (41, 80),    # very large — stress backpressure
        (81, 150),   # huge — heavy backpressure
    ]

    generated = []
    for i in range(count):
        profile = random.choice(profiles)
        min_l, max_l = random.choice(lengths)

        program = gen_program(min_len=min_l, max_len=max_l, profile=profile)

        filename = os.path.join(output_dir, f"fuzz_{i:04d}.json")
        with open(filename, 'w') as f:
            json.dump(program, f, indent=2)

        generated.append((filename, profile, len(program)))

    return generated


def run_test(input_file, sim_path, ref_sim_path, output_dir):
    """Run a single test through C++ simulator and Python reference, compare."""
    base = os.path.splitext(os.path.basename(input_file))[0]
    sim_output = os.path.join(output_dir, f"{base}_sim.json")
    ref_output = os.path.join(output_dir, f"{base}_ref.json")

    # Run your C++ simulator
    try:
        result = subprocess.run(
            [sim_path, input_file, sim_output],
            capture_output=True, text=True, timeout=30
        )
        if result.returncode != 0:
            return "SIM_CRASH", result.stderr[:200]
    except subprocess.TimeoutExpired:
        return "SIM_TIMEOUT", ""

    # Run Python reference simulator
    try:
        result = subprocess.run(
            ["python3", ref_sim_path, input_file, ref_output],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode != 0:
            return "REF_CRASH", result.stderr[:200]
    except subprocess.TimeoutExpired:
        return "REF_TIMEOUT", ""

    # Compare JSON outputs directly
    try:
        with open(sim_output) as f:
            sim_data = json.load(f)
        with open(ref_output) as f:
            ref_data = json.load(f)

        if len(sim_data) != len(ref_data):
            return "MISMATCH", f"Cycle count: sim={len(sim_data)} ref={len(ref_data)}"

        for i in range(len(ref_data)):
            for key in ref_data[i]:
                if key not in sim_data[i]:
                    return "MISMATCH", f"Cycle {i}: missing key '{key}'"
                if ref_data[i][key] != sim_data[i][key]:
                    return "MISMATCH", (
                        f"Cycle {i}, key '{key}':\n"
                        f"  ref={ref_data[i][key]}\n"
                        f"  sim={sim_data[i][key]}"
                    )

        return "PASS", ""

    except Exception as e:
        return "COMPARE_ERROR", str(e)


def main():
    parser = argparse.ArgumentParser(description="OoO470 Processor Simulator Fuzzer")
    parser.add_argument("--generate", type=int, default=100,
                        help="Number of test programs to generate")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed for reproducibility")
    parser.add_argument("--output-dir", type=str, default="tests_fuzz",
                        help="Directory for generated tests")
    parser.add_argument("--run", action="store_true",
                        help="Run tests after generating")
    parser.add_argument("--sim", type=str, default="./simulator",
                        help="Path to your C++ simulator")
    parser.add_argument("--ref", type=str, default="./ref_sim.py",
                        help="Path to ref_sim.py (Python reference simulator)")

    args = parser.parse_args()

    print(f"Generating {args.generate} random test programs...")
    tests = generate_tests(args.output_dir, args.generate, args.seed)
    print(f"Generated {len(tests)} tests in {args.output_dir}/")

    # Print profile distribution
    from collections import Counter
    profile_counts = Counter(t[1] for t in tests)
    print("\nProfile distribution:")
    for profile, count in sorted(profile_counts.items()):
        print(f"  {profile}: {count}")

    length_stats = [t[2] for t in tests]
    print(f"\nProgram lengths: min={min(length_stats)}, max={max(length_stats)}, "
          f"avg={sum(length_stats)/len(length_stats):.1f}")

    if not args.run:
        print(f"\nTo run tests against Python reference:")
        print(f"  python3 fuzzer.py --generate {args.generate} --run "
              f"--sim {args.sim} --ref {args.ref}")
        return

    # Run all tests
    print(f"\nRunning {len(tests)} tests...")
    print(f"  C++ simulator: {args.sim}")
    print(f"  Python reference: {args.ref}")
    results = {"PASS": 0, "MISMATCH": 0, "SIM_CRASH": 0, "SIM_TIMEOUT": 0,
               "REF_CRASH": 0, "REF_TIMEOUT": 0, "COMPARE_ERROR": 0}
    mismatches = []

    for i, (filename, profile, length) in enumerate(tests):
        status, detail = run_test(filename, args.sim, args.ref, args.output_dir)
        results[status] += 1

        if status != "PASS":
            mismatches.append((filename, profile, length, status, detail))
            print(f"  [{i+1}/{len(tests)}] FAIL ({status}): {filename} "
                  f"[profile={profile}, len={length}]")
        elif (i + 1) % 25 == 0:
            print(f"  [{i+1}/{len(tests)}] {results['PASS']} passed so far...")

    # Summary
    print(f"\n{'='*60}")
    print(f"RESULTS: {results['PASS']}/{len(tests)} passed")
    for status, count in results.items():
        if status != "PASS" and count > 0:
            print(f"  {status}: {count}")

    if mismatches:
        print(f"\nFailing tests:")
        for filename, profile, length, status, detail in mismatches:
            print(f"  {filename} (profile={profile}, len={length}, status={status})")
            if detail:
                print(f"    {detail[:200]}")
        print(f"\nTo reproduce a specific failure:")
        print(f"  {args.sim} <failing_test.json> my_output.json")
        print(f"  python3 {args.ref} <failing_test.json> ref_output.json")
        print(f"  # Then diff my_output.json ref_output.json")
    else:
        print("\nAll tests passed!")


if __name__ == "__main__":
    main()
