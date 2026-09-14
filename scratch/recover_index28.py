#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scan_gmod_hooks import (
    ExactFp,
    GMOD,
    SRC,
    at,
    find_bytes,
    load_exact,
    load_minhooks,
    load_normalized,
    load_pe,
    scan_exact,
)

def score_body(data: bytes, fp: ExactFp) -> int:
    n = 0
    for i, rel in enumerate(fp.offsets):
        if rel < len(data) and data[rel] == fp.values[i]:
            n += 1
    return n


def main() -> None:
    exact = load_exact(SRC / "kirkware_hook_fingerprints.hpp")
    hooks = load_minhooks(SRC / "install_kirkware_game_hooks.cpp")
    client = load_pe(GMOD / "client.dll")
    engine = load_pe(GMOD / "engine.dll")
    fp = exact[28]
    print("index 28 prefix", fp.prefix.hex(" "), "offs", fp.offsets, "vals", fp.values.hex(" "))

    prefix_hits = find_bytes(client, fp.prefix)
    print("prefix hits", len(prefix_hits))
    scored = []
    for rva in prefix_hits:
        data = at(client, rva, max(fp.offsets) + 1)
        if not data:
            continue
        s = score_body(data, fp)
        if s >= 6:
            scored.append((s, rva, data[:16].hex(" ")))
    scored.sort(reverse=True)
    print("top prefix+body scores:")
    for s, rva, hx in scored[:15]:
        rf = rva in client.runtime_starts
        print(f"  score={s:2d}/12 rva=0x{rva:X} rf={int(rf)} {hx}")

    print("\nexpected +0x70 window:")
    for rva in range(0xDF000, 0xDF200, 0x10):
        data = at(client, rva, 16)
        print(f"  0x{rva:X}: {data.hex(' ') if data else 'OOB'}")

    print("\nRF starts near 0xDF000:")
    for rva in client.runtime_starts:
        if 0xDEE00 <= rva <= 0xDF400:
            data = at(client, rva, 24)
            print(f"  rf 0x{rva:X}: {data.hex(' ') if data else 'OOB'}")

    # retune existing offsets at +0x70 candidate
    cand = 0xDF080
    data = at(client, cand, max(fp.offsets) + 1)
    print(f"\nbytes at 0x{cand:X} len={len(data) if data else 0}")
    if data:
        print(data[:64].hex(" "))
        new_vals = bytes(data[o] for o in fp.offsets)
        new_fp = ExactFp(fp.prefix, fp.prefix_mask, fp.offsets, new_vals)
        hits = scan_exact(client, new_fp)
        print("retuned offsets at DF080 unique?", len(hits), [hex(x) for x in hits[:6]])

    # CCL
    ccl = bytes([
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41,
        0x8B, 0x40, 0x08, 0x49, 0x8B, 0xD8, 0x4D, 0x8B,
        0x08, 0x8B, 0xFA, 0x89, 0x44, 0x24, 0x20, 0x48,
        0x8B, 0xF1,
    ])
    print("\nCCL prefix32", [hex(x) for x in find_bytes(client, ccl)])
    repl = bytes([
        0x44, 0x8B, 0xCA, 0x4C, 0x8B, 0xC1, 0xB8, 0xB7,
        0x60, 0x0B, 0xB6, 0x41, 0x8B, 0xC9, 0xF7, 0xEA,
    ])
    print("replacement prefix", [hex(x) for x in find_bytes(client, repl)])

    # engine 95: E9 + 7 CC near 0x797A0
    print("\nengine E9+CC near 0x79000-0x7A000")
    data = engine.sections[0].data
    base = engine.sections[0].rva
    found = []
    for off in range(len(data) - 12):
        if data[off] == 0xE9 and data[off + 5 : off + 12] == b"\xCC" * 7:
            rva = base + off
            if 0x78000 <= rva <= 0x7B000:
                found.append(rva)
    print([hex(x) for x in found[:20]], "count_in_window", len(found))

    # vgui dump
    vgui = load_pe(GMOD / "vgui2.dll")
    vgui_p = bytes([0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01])
    print("\nvgui prefix hits:")
    for rva in find_bytes(vgui, vgui_p):
        print(f"  0x{rva:X} {at(vgui, rva, 24).hex(' ')}")


if __name__ == "__main__":
    main()
