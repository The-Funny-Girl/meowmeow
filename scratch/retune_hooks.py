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
    scan_norm,
)


def fmt_bytes(b: bytes) -> str:
    return ", ".join(f"0x{x:02X}" for x in b)


def fmt_offs(offs) -> str:
    return ", ".join(f"0x{x:02X}" for x in offs)


def masked_hits(pe, rva, span, zero_offs):
    data = at(pe, rva, span)
    if not data or len(data) < span:
        return []
    hits = []
    needle = data
    for sec in pe.sections:
        blob = sec.data
        for off in range(len(blob) - span + 1):
            ok = True
            for i in range(span):
                if i in zero_offs:
                    continue
                if blob[off + i] != needle[i]:
                    ok = False
                    break
            if ok:
                hits.append(sec.rva + off)
    return hits


def retune_values(pe, fp: ExactFp, rva: int) -> ExactFp | None:
    need = max(fp.offsets) + 1
    data = at(pe, rva, need)
    if not data or len(data) < need:
        return None
    if data[:5] != fp.prefix:
        prefix = data[:5]
    else:
        prefix = fp.prefix
    values = bytes(data[o] for o in fp.offsets)
    return ExactFp(prefix, fp.prefix_mask, fp.offsets, values)


def greedy_fp(pe, rva: int, span: int = 0x80) -> ExactFp | None:
    data = at(pe, rva, span)
    if not data or len(data) < 16:
        return None
    prefix = data[:5]
    candidates = find_bytes(pe, prefix)
    if rva not in candidates:
        return None
    # pick offsets that split the candidate set
    remain = set(candidates)
    chosen: list[int] = []
    values: list[int] = []
    # skip likely rel32 ranges
    skip = set()
    i = 0
    while i < len(data) - 4:
        b = data[i]
        if b in (0xE8, 0xE9):
            skip.update(range(i + 1, i + 5))
            i += 5
            continue
        if i + 6 < len(data) and data[i : i + 3] in (
            b"\x48\x8B\x0D",
            b"\x48\x8D\x0D",
            b"\x48\x8B\x05",
            b"\x48\x8D\x15",
            b"\x4C\x8D\x05",
            b"\x4C\x8B\x05",
        ):
            skip.update(range(i + 3, i + 7))
            i += 7
            continue
        if i + 6 < len(data) and data[i : i + 2] == b"\x80\x3D":
            skip.update(range(i + 2, i + 6))
            i += 7
            continue
        i += 1
    for _ in range(12):
        best = None
        best_score = -1
        for off in range(5, len(data)):
            if off in chosen or off in skip:
                continue
            groups = {}
            for cand in remain:
                b = at(pe, cand + off, 1)
                if not b:
                    continue
                groups.setdefault(b[0], set()).add(cand)
                if rva in groups.get(b[0], ()):
                    score = len(remain) - len(groups[b[0]])
                    if score > best_score:
                        best_score = score
                        best = off
        if best is None:
            break
        chosen.append(best)
        values.append(at(pe, rva + best, 1)[0])
        keep = set()
        for cand in remain:
            b = at(pe, cand + best, 1)
            if b and b[0] == values[-1]:
                keep.add(cand)
        remain = keep
        if remain == {rva}:
            break
    while len(chosen) < 12:
        for off in range(5, len(data)):
            if off not in chosen and off not in skip:
                chosen.append(off)
                values.append(data[off])
                break
        else:
            return None
    fp = ExactFp(prefix, bytes([0xFF] * 5), chosen, bytes(values))
    hits = scan_exact(pe, fp)
    if hits != [rva]:
        return None
    return fp


def ccl_match(bytes_: bytes) -> bool:
    fp = bytes([
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41,
        0x8B, 0x40, 0x08, 0x49, 0x8B, 0xD8, 0x4D, 0x8B,
        0x08, 0x8B, 0xFA, 0x89, 0x44, 0x24, 0x20, 0x48,
        0x8B, 0xF1, 0xE8, 0x59, 0xE1, 0xFF, 0xFF, 0x48,
        0x8B, 0x5B, 0x18, 0x48, 0x85, 0xDB, 0x74, 0x20,
        0x8B, 0x43, 0x08, 0x4C, 0x8B, 0xC3, 0x4C, 0x8B,
        0x0B, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0x89, 0x44,
        0x24, 0x20, 0xE8, 0x39, 0xE1, 0xFF, 0xFF, 0x48,
        0x8B, 0x5B, 0x18, 0x48, 0x85, 0xDB, 0x75, 0xE0,
        0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x8B, 0x74,
        0x24, 0x48, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3,
    ])
    if len(bytes_) < 96:
        return False
    for i in range(96):
        relative = (0x23 <= i <= 0x26) or (0x43 <= i <= 0x46) or (0x30 <= i <= 0x35)
        if not relative and bytes_[i] != fp[i]:
            return False
    import struct
    first = struct.unpack_from("<i", bytes_, 0x23)[0]
    second = struct.unpack_from("<i", bytes_, 0x43)[0]
    return first == second + 0x20


def main() -> None:
    exact = load_exact(SRC / "kirkware_hook_fingerprints.hpp")
    norm, pattern, masks = load_normalized(SRC / "kirkware_hook_normalized.hpp")
    hooks = load_minhooks(SRC / "install_kirkware_game_hooks.cpp")
    pes = {
        "Client": load_pe(GMOD / "client.dll"),
        "Engine": load_pe(GMOD / "engine.dll"),
        "LuaShared": load_pe(GMOD / "lua_shared.dll"),
    }

    resolved = {}
    for i, (h, e, n) in enumerate(zip(hooks, exact, norm)):
        pe = pes[h.module]
        eh = scan_exact(pe, e)
        if len(eh) == 1:
            resolved[i] = eh[0]
            continue
        if n.domain != "CurrentExactOnly":
            nh = scan_norm(pe, n, pattern, masks)
            if len(nh) == 1:
                resolved[i] = nh[0]
                continue
        if i == 28:
            resolved[i] = 0xDF080

    print("RESOLVED", len(resolved))
    for i in range(69):
        if i not in resolved:
            print("MISSING", i)

    print("\nEXACT RETUNES")
    new_exact = list(exact)
    for i, (h, e) in enumerate(zip(hooks, exact)):
        pe = pes[h.module]
        rva = resolved[i]
        tuned = retune_values(pe, e, rva)
        if tuned is None:
            print(i, "cannot retune values")
            g = greedy_fp(pe, rva)
            print("  greedy", g)
            if g:
                new_exact[i] = g
            continue
        hits = scan_exact(pe, tuned)
        changed = tuned.values != e.values or tuned.prefix != e.prefix
        unique = hits == [rva]
        status = "unique" if unique else f"hits={len(hits)} { [hex(x) for x in hits[:4]] }"
        if not unique:
            g = greedy_fp(pe, rva)
            print(f"  {i:02d} values-not-unique {status} greedy={'ok' if g else 'FAIL'}")
            if g:
                new_exact[i] = g
                changed = True
                unique = True
        else:
            new_exact[i] = tuned
        if changed:
            print(f"  {i:02d} 0x{rva:X} {status} prefix={tuned.prefix.hex()} vals={tuned.values.hex()}")

    print("\nINDEX 28 MASKED 61")
    client = pes["Client"]
    zero = set(range(0x16, 0x1A)) | set(range(0x23, 0x27)) | set(range(0x2D, 0x31)) | set(range(0x32, 0x36))
    hits = masked_hits(client, 0xDF080, 0x3D, zero)
    print("hits", len(hits), [hex(x) for x in hits[:8]])
    data = at(client, 0xDF080, 0x3D)
    print("bytes", data.hex(" "))

    print("\nCCL")
    ccl_hits = []
    sec = client.sections[0]
    for off in range(len(sec.data) - 95):
        if ccl_match(sec.data[off : off + 96]):
            ccl_hits.append(sec.rva + off)
    print("ccl matches", len(ccl_hits), [hex(x) for x in ccl_hits[:6]])
    if ccl_hits:
        blob = at(client, ccl_hits[0], 96)
        print("first 40", blob[:40].hex(" "))

    print("\nNEW BASELINES")
    for i in range(69):
        print(f"{i:02d} 0x{resolved[i]:06X}")

    print("\nC++ exact lines that change:")
    for i, (old, new) in enumerate(zip(exact, new_exact)):
        if old.prefix != new.prefix or old.offsets != new.offsets or old.values != new.values:
            print(
                f"    // {i}\n"
                f"    {{{{{fmt_bytes(new.prefix)}}}, {{{fmt_bytes(new.prefix_mask)}}}, "
                f"{{{fmt_offs(new.offsets)}}}, {{{fmt_bytes(new.values)}}}}},"
            )


if __name__ == "__main__":
    main()
