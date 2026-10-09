#!/usr/bin/env python3
"""Turn an objdump disassembly into the static instruction table used by a1time.

Usage: gen_pcinfo.py program.elf out.bin
"""

import re
import struct
import subprocess
import sys

CLS = {name: i for i, name in enumerate([
    "alu", "f1", "f2", "mul", "f3", "f4", "div", "fdiv", "load", "store",
    "branch", "jump", "call", "ret", "nop"])}

ALU = set("""add addpchi addpc ldi mov and or xor sub sne seq slt sltu sle sleu lsl lsr asr
ebf ebfu mkbf ibf sel sel.132 shuf max min maxu minu and.np or.np clz pack pack.b pack.h packhi
packhi.b packhi.h seq.b ldea xchgsr getsr setsr nop popcnt rev crc32c crc32""".split())
F1 = set("fslt fseq fsne fsle fmin fmax fsunord fsord frsqrta frecipa".split())
SAU = set("adds addsu subs subsu addh addhu subh subhu mulq mulqr".split())
MUL = set("mul mulhi mulhiu madd".split())
F3 = set("itof utof ftoi ftou ftoir ftour".split())
F4 = set("fadd fsub fmul".split())
DIV = set("div divu rem remu".split())
LOAD = {"ldw": 4, "ldub": 1, "ldb": 1, "ldh": 2, "lduh": 2}
STORE = {"stw": 4, "stb": 1, "sth": 2}
LOAD.update({k + "pc": v for k, v in list(LOAD.items())})
STORE.update({k + "pc": v for k, v in list(STORE.items())})
BRANCH = set("bz bnz bs bns blt bge ble bgt bltz bgez blez bgtz".split())
SRC_IS_DST = set("madd ibf sel sel.132".split())

REG_NAMES = {"z": 0, "tp": 27, "fp": 28, "sp": 29, "lr": 30, "vl": 31}
NOREG = 255


def reg(tok):
    """Return (index, is_vector) or None."""
    tok = tok.strip()
    m = re.fullmatch(r"r(\d+)", tok)
    if m:
        return int(m.group(1)), False
    if tok in REG_NAMES:
        return REG_NAMES[tok], False
    m = re.fullmatch(r"v(\d+)", tok)
    if m:
        return int(m.group(1)), True
    if tok == "vz":
        return 0, True
    return None


def parse_imm(tok):
    m = re.match(r"#(-?(?:0x[0-9a-f]+|\d+))", tok.strip())
    return int(m.group(1), 0) if m else None


def classify(mn):
    base = mn.split("/")[0]
    if base in LOAD:
        return "load"
    if base in STORE:
        return "store"
    if base in BRANCH:
        return "branch"
    if base == "b":
        return "jump"
    if base == "j":
        return "jump"
    if base in ("jl", "bl"):
        return "call"
    if base == "ret":
        return "ret"
    if base in F1:
        return "f1"
    if base in SAU:
        return "f2"
    if base in MUL:
        return "mul"
    if base in F3:
        return "f3"
    if base in F4:
        return "f4"
    if base in DIV:
        return "div"
    if base == "fdiv":
        return "fdiv"
    if base.split(".")[0] in ALU or base in ALU:
        return "alu"
    return None


def split_operands(ops):
    out, depth, cur = [], 0, ""
    for ch in ops:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def main():
    elf, out_path = sys.argv[1:3]
    dis = subprocess.run(["mrisc32-elf-objdump", "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = subprocess.run(["mrisc32-elf-nm", "-n", "--defined-only", elf],
                          capture_output=True, text=True, check=True).stdout
    funcs = []
    for line in syms.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "Tt":
            funcs.append((int(parts[0], 16), parts[2]))

    entries = {}
    unknown = {}
    for line in dis.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        mn = m.group(2)
        ops_text = m.group(3).split(";")[0].strip()
        ops = split_operands(ops_text) if ops_text else []
        cls = classify(mn)
        if cls is None:
            unknown[mn] = unknown.get(mn, 0) + 1
            cls = "alu"
        dst, dst_vec = NOREG, 0
        srcs = []
        size, scale, imm, mode = 0, 1, 0, 0
        if cls in ("load", "store"):
            size = (LOAD if cls == "load" else STORE)[mn]
            data = reg(ops[0])
            if mn.endswith("pc"):
                m = re.search(r"#\((0x[0-9a-f]+)", ops[1])
                imm = int(m.group(1), 16) - pc if m else 0
                mode = 3
                if cls == "load":
                    dst, dst_vec = data[0], int(data[1])
                else:
                    srcs.append(data)
                mem = None
            else:
                mem = ops[1].strip("[]")
            parts = [p.strip() for p in mem.split(",")] if mem else []
            if parts:
                base = reg(parts[0])
                srcs.append(base)
            if len(parts) > 1:
                idx = parts[1]
                if idx.startswith("#"):
                    imm = parse_imm(idx)
                    mode = 0
                else:
                    sm = re.fullmatch(r"(\S+?)\*(\d+)", idx)
                    if sm:
                        idx, scale = sm.group(1), int(sm.group(2))
                    r = reg(idx)
                    srcs.append(r)
                    mode = 2 if r[1] else 1
            if mem is not None:
                if cls == "load":
                    dst, dst_vec = data[0], int(data[1])
                else:
                    srcs.append(data)
        elif cls == "branch":
            srcs.append(reg(ops[0]))
        elif cls in ("jump", "call"):
            if mn in ("b", "bl"):
                if mn == "bl":
                    dst = 30
            else:
                srcs.append(reg(ops[0]))
                if cls == "call":
                    dst = 30
        elif cls == "ret":
            srcs.append((30, False))
        else:
            if mn == "ldea":
                d = reg(ops[0])
                dst, dst_vec = d[0], int(d[1])
                mem = ops[1].strip("[]")
                for p in mem.split(","):
                    p = p.strip().split("*")[0]
                    r = reg(p)
                    if r:
                        srcs.append(r)
            elif ops:
                d = reg(ops[0])
                if d:
                    dst, dst_vec = d[0], int(d[1])
                    if mn in SRC_IS_DST:
                        srcs.append(d)
                for o in ops[1:]:
                    r = reg(o)
                    if r:
                        srcs.append(r)
        if dst == 0 and not dst_vec:
            dst = NOREG
        srcs = [s for s in srcs if s and not (s[0] == 0 and not s[1])][:3]
        src_regs = [s[0] for s in srcs] + [NOREG] * (3 - len(srcs))
        src_vec = sum((1 << i) for i, s in enumerate(srcs) if s[1])
        is_vector = dst_vec or src_vec
        entries[pc] = (CLS[cls], dst, dst_vec, src_regs, src_vec, int(bool(is_vector)),
                       size, scale, mode, imm)

    lo, hi = min(entries), max(entries) + 4
    fidx = 0
    names = [f for _, f in funcs]
    with open(out_path, "wb") as f:
        f.write(struct.pack("<III", lo, (hi - lo) // 4, len(names)))
        for name in names:
            b = name.encode()[:63]
            f.write(struct.pack("<B", len(b)) + b)
        for pc in range(lo, hi, 4):
            while fidx + 1 < len(funcs) and funcs[fidx + 1][0] <= pc:
                fidx += 1
            e = entries.get(pc)
            if e is None:
                f.write(struct.pack("<BBBBBBBBBBBBiI", CLS["nop"], NOREG, 0, NOREG, NOREG, NOREG,
                                    0, 0, 0, 1, 0, 0, 0, fidx))
            else:
                cls, dst, dst_vec, s, src_vec, is_vec, size, scale, mode, imm = e
                f.write(struct.pack("<BBBBBBBBBBBBiI", cls, dst, dst_vec, s[0], s[1], s[2],
                                    src_vec, is_vec, size, scale, mode, 0, imm, fidx))
    if unknown:
        print("unclassified (treated as alu):", unknown, file=sys.stderr)


if __name__ == "__main__":
    main()
