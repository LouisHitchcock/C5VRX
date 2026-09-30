#!/usr/bin/env python3
"""Read-only, pinned C5 libphy AGC/watchdog audit; never modifies the archive.

Needs GNU RISC-V objdump (Espressif's or upstream's). Pass --objdump or put
riscv32-esp-elf-objdump/riscv64-linux-gnu-objdump on PATH. No Python packages.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


PIN = "59c1234e929212aec0fdda75769b759951235536"
SHA256 = "dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb"
TARGETS = (
    "phy_agc_reg_init_new", "phy_agc_max_gain_set", "bb_agc_reg_update",
    "phy_set_rx_comp_new", "phy_enable_agc", "phy_disable_agc",
    "phy_force_rx_gain", "phy_rfagc_disable", "phy_rx_pkdet_num_set",
    "phy_bb_wdg_cfg", "phy_set_bb_wdg", "phy_bb_wdt_rst_enable",
    "phy_bb_wdt_int_enable", "phy_bb_wdt_timeout_clear",
    "phy_bb_wdt_get_status", "phy_bb_fsm_rst", "phy_reg_init_new",
    "phy_wifi_agc_sat_gain", "phy_rx_sense_set",
)
HEADER = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")
MEMBER = re.compile(r"^(\S+\.o):\s+file format (\S+)")
CALL = re.compile(r"R_RISCV_(?:CALL|CALL_PLT|JAL)\s+(\S+)")


def elf_bindings(symbols, disassembly):
    """Report final symbol locations and visible direct calls, not ROM internals."""
    result = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) < 4 or parts[-1] not in TARGETS:
            continue
        if not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        address, section = int(parts[0], 16), parts[-3]
        kind = ("rom_address" if 0x40000000 <= address < 0x40020000 else
                "undefined" if section == "*UND*" else "linked_code")
        result[parts[-1]] = dict(address=f"0x{address:08x}", section=section,
                                kind=kind, call_sites=[])
    caller = None
    for line in disassembly.splitlines():
        header = HEADER.match(line)
        if header:
            caller = header[2]
        if not re.search(r"\b(?:jal|jalr|j|jr)\b", line):
            continue
        target = re.search(r"<([^>]+)>", line)
        if target and target[1] in result:
            result[target[1]]["call_sites"].append(dict(caller=caller, instruction=line.strip()))
    return {name: result.get(name, dict(kind="not_in_symbol_table")) for name in TARGETS}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("archive", type=Path)
    p.add_argument("--objdump")
    p.add_argument("--elf", type=Path,
                   help="Also audit final firmware symbol bindings and visible call sites")
    p.add_argument("--allow-other-hash", action="store_true",
                   help="Report an unpinned archive; it has no validated field semantics")
    p.add_argument("--disassembly", action="store_true",
                   help="Include focused instruction/relocation evidence in the JSON")
    args = p.parse_args()
    digest = hashlib.sha256(args.archive.read_bytes()).hexdigest()
    if digest != SHA256 and not args.allow_other_hash:
        p.error(f"Archive hash {digest} differs from the pinned C5 libphy; refusing")
    tool = args.objdump or next((x for name in (
        "riscv32-esp-elf-objdump", "riscv32-unknown-elf-objdump",
        "riscv64-linux-gnu-objdump") if (x := shutil.which(name))), None)
    if not tool:
        p.error("GNU RISC-V objdump is required; pass --objdump")
    raw = subprocess.run([tool, "-dr", str(args.archive)], check=True,
                         capture_output=True, text=True).stdout
    member = function = None
    functions, calls = {}, []
    for line in raw.splitlines():
        m = MEMBER.match(line)
        if m:
            member, fmt = m.groups()
            if fmt != "elf32-littleriscv":
                p.error(f"Unexpected member format: {fmt}")
            function = None
        m = HEADER.match(line)
        if m and not m[2].startswith((".", "$")):
            function = m[2]
            if function in TARGETS:
                functions[function] = {"object": member, "lines": []}
        if function in TARGETS:
            functions[function]["lines"].append(line)
        m = CALL.search(line)
        if m and (target := m[1].split("+")[0]) in TARGETS:
            calls.append({"object": member, "caller": function,
                          "target": target, "relocation": line.strip()})
    missing = sorted(set(TARGETS) - functions.keys())
    if missing:
        p.error("Required functions missing: " + ", ".join(missing))
    if not args.disassembly:
        functions = {k: {"object": v["object"]} for k, v in functions.items()}
    elf = None
    if args.elf:
        header = subprocess.run([tool, "-f", str(args.elf)], check=True,
                                capture_output=True, text=True).stdout
        if "file format elf32-littleriscv" not in header:
            p.error("Expected a linked RV32 firmware ELF")
        symbols = subprocess.run([tool, "-t", str(args.elf)], check=True,
                                 capture_output=True, text=True).stdout
        code = subprocess.run([tool, "-d", str(args.elf)], check=True,
                              capture_output=True, text=True).stdout
        elf = dict(sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                   bindings=elf_bindings(symbols, code))
    print(json.dumps({
        "archive_sha256": digest, "pinned": digest == SHA256,
        "esp_phy_lib_commit": PIN if digest == SHA256 else None,
        "objdump_version": subprocess.run([tool, "--version"], check=True,
            capture_output=True, text=True).stdout.splitlines()[0],
        "functions": functions, "calls": calls,
        "firmware_elf": elf,
        "limits": ["Archive evidence only; final ELF/ROM bindings must be checked",
                   "Register writes do not prove watchdog causes live acquisitions",
                   "ROM-address binding does not reveal ROM-internal calls or implementation",
                   "No archive mutation and no firmware/hardware execution"],
    }, indent=2))


if __name__ == "__main__":
    main()
