#!/usr/bin/env python3
"""Offline scan of live GMod DLLs against kirkware MinHook fingerprints."""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from pathlib import Path

GMOD = Path(r"F:\SteamLibrary\steamapps\common\GarrysMod\bin\win64")
SRC = Path(r"E:\SSORIG\Kirkware\source\core")
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3


@dataclass
class ExactFp:
    prefix: bytes
    prefix_mask: bytes
    offsets: list[int]
    values: bytes


@dataclass
class NormFp:
    blob_offset: int
    span: int
    fixed_bytes: int
    baseline_rva: int
    domain: str
    anchor_offset: int
    anchor_length: int


@dataclass
class MinHook:
    saved: int
    module: str
    baseline: int
    detour: int


@dataclass
class Section:
    rva: int
    data: bytes


@dataclass
class PeImage:
    path: Path
    size: int
    image: bytearray
    sections: list[Section]
    runtime_starts: list[int]


def parse_hex_bytes(text: str) -> bytes:
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", text))


def parse_int_list(text: str) -> list[int]:
    vals = []
    for tok in re.findall(r"0x[0-9A-Fa-f]+|\d+", text):
        vals.append(int(tok, 0))
    return vals


def load_exact(path: Path) -> list[ExactFp]:
    text = path.read_text(encoding="utf-8")
    start = text.find("kMinHookTargets{{")
    end = text.find("}};", start)
    body = text[start:end]
    fps = []
    for m in re.finditer(
        r"\{\{\{([^}]+)\}\},\s*\{\{([^}]+)\}\},\s*\{\{([^}]+)\}\},\s*\{\{([^}]+)\}\}\}",
        body,
    ):
        fps.append(
            ExactFp(
                prefix=parse_hex_bytes(m.group(1)),
                prefix_mask=parse_hex_bytes(m.group(2)),
                offsets=parse_int_list(m.group(3)),
                values=parse_hex_bytes(m.group(4)),
            )
        )
    if len(fps) != 69:
        raise SystemExit(f"exact fingerprints: {len(fps)}")
    return fps


def load_normalized(path: Path) -> tuple[list[NormFp], bytes, bytes]:
    text = path.read_text(encoding="utf-8")
    start = text.find("kMinHookTargets{{")
    end = text.find("}};", start)
    body = text[start:end]
    fps = []
    for m in re.finditer(
        r"\{(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*0x([0-9A-Fa-f]+)u,\s*CandidateDomain::(\w+),\s*(\d+)u,\s*(\d+)u\}",
        body,
    ):
        fps.append(
            NormFp(
                blob_offset=int(m.group(1)),
                span=int(m.group(2)),
                fixed_bytes=int(m.group(3)),
                baseline_rva=int(m.group(4), 16),
                domain=m.group(5),
                anchor_offset=int(m.group(6)),
                anchor_length=int(m.group(7)),
            )
        )
    if len(fps) != 69:
        raise SystemExit(f"normalized fingerprints: {len(fps)}")
    def load_blob(name: str) -> bytes:
        s = text.find(f"{name}{{")
        e = text.find("}};", s)
        return parse_hex_bytes(text[s:e])

    pattern = load_blob("kPatternBytes")
    masks = load_blob("kPatternMasks")
    if len(pattern) != len(masks):
        raise SystemExit(f"blob size mismatch {len(pattern)} vs {len(masks)}")
    return fps, pattern, masks


def load_minhooks(path: Path) -> list[MinHook]:
    text = path.read_text(encoding="utf-8")
    start = text.find("kMinHooks{{")
    end = text.find("}};", start)
    body = text[start:end]
    hooks = []
    for m in re.finditer(
        r"\{0x([0-9A-Fa-f]+),\s*HookModule::(\w+),\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\}",
        body,
    ):
        hooks.append(
            MinHook(
                saved=int(m.group(1), 16),
                module=m.group(2),
                baseline=int(m.group(3), 16),
                detour=int(m.group(4), 16),
            )
        )
    if len(hooks) != 69:
        raise SystemExit(f"minhooks: {len(hooks)}")
    return hooks


def load_pe(path: Path) -> PeImage:
    raw = path.read_bytes()
    e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    magic = struct.unpack_from("<H", raw, e_lfanew + 24)[0]
    if magic != 0x20B:
        raise SystemExit(f"{path} is not PE32+")
    size_of_image = struct.unpack_from("<I", raw, e_lfanew + 24 + 56)[0]
    num_sections = struct.unpack_from("<H", raw, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", raw, e_lfanew + 20)[0]
    section_off = e_lfanew + 24 + opt_size
    num_rva = struct.unpack_from("<I", raw, e_lfanew + 24 + 108)[0]
    image = bytearray(size_of_image)
    sections: list[Section] = []
    for i in range(num_sections):
        rec = section_off + i * 40
        name = raw[rec : rec + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        vsize, va, raw_size, raw_ptr, _, _, _, _, chars = struct.unpack_from(
            "<IIIIIIHHI", raw, rec + 8
        )
        n = min(raw_size, vsize if vsize else raw_size, size_of_image - va)
        if n > 0 and raw_ptr + n <= len(raw) and va + n <= size_of_image:
            image[va : va + n] = raw[raw_ptr : raw_ptr + n]
        if chars & IMAGE_SCN_MEM_EXECUTE:
            extent = vsize or raw_size
            if va + extent > size_of_image:
                extent = size_of_image - va
            sections.append(Section(va, bytes(image[va : va + extent])))
    runtime_starts: list[int] = []
    if num_rva > IMAGE_DIRECTORY_ENTRY_EXCEPTION:
        exc_rva, exc_size = struct.unpack_from(
            "<II", raw, e_lfanew + 24 + 112 + IMAGE_DIRECTORY_ENTRY_EXCEPTION * 8
        )
        if exc_rva and exc_size >= 12 and exc_rva + exc_size <= size_of_image:
            blob = bytes(image[exc_rva : exc_rva + exc_size])
            for off in range(0, len(blob) - 11, 12):
                begin, end, _ = struct.unpack_from("<III", blob, off)
                if begin < end <= size_of_image:
                    runtime_starts.append(begin)
            runtime_starts = sorted(set(runtime_starts))
    return PeImage(path, size_of_image, image, sections, runtime_starts)


def prefix_ok(data: bytes, off: int, fp: ExactFp) -> bool:
    if off + 5 > len(data):
        return False
    for i in range(5):
        if (data[off + i] & fp.prefix_mask[i]) != (fp.prefix[i] & fp.prefix_mask[i]):
            return False
    return True


def body_ok(data: bytes, off: int, fp: ExactFp) -> bool:
    for i, rel in enumerate(fp.offsets):
        if off + rel >= len(data) or data[off + rel] != fp.values[i]:
            return False
    return True


def scan_exact(pe: PeImage, fp: ExactFp) -> list[int]:
    hits: list[int] = []
    last = fp.offsets[-1]
    full_prefix = all(b == 0xFF for b in fp.prefix_mask)
    for aligned in (True, False):
        hits.clear()
        for sec in pe.sections:
            data = sec.data
            if len(data) <= last:
                continue
            if full_prefix:
                start = 0
                while True:
                    off = data.find(fp.prefix, start)
                    if off < 0 or off + last >= len(data):
                        break
                    rva = sec.rva + off
                    if ((rva & 15) == 0) == aligned and body_ok(data, off, fp):
                        hits.append(rva)
                    start = off + 1
            else:
                limit = len(data) - last
                for off in range(limit):
                    rva = sec.rva + off
                    if ((rva & 15) == 0) != aligned:
                        continue
                    if (data[off] & fp.prefix_mask[0]) != (
                        fp.prefix[0] & fp.prefix_mask[0]
                    ):
                        continue
                    if not body_ok(data, off, fp):
                        continue
                    if not prefix_ok(data, off, fp):
                        continue
                    hits.append(rva)
        if hits:
            return hits
    return hits


def norm_ok(data: bytes, off: int, fp: NormFp, pattern: bytes, masks: bytes) -> bool:
    if off + fp.span > len(data):
        return False
    blob = fp.blob_offset
    anchor = off + fp.anchor_offset
    if data[anchor : anchor + fp.anchor_length] != pattern[blob + fp.anchor_offset : blob + fp.anchor_offset + fp.anchor_length]:
        return False
    for i in range(fp.span):
        mask = masks[blob + i]
        if (data[off + i] & mask) != (pattern[blob + i] & mask):
            return False
    return True


def scan_norm(pe: PeImage, fp: NormFp, pattern: bytes, masks: bytes) -> list[int]:
    hits: list[int] = []
    if fp.domain == "CurrentExactOnly":
        return hits
    if fp.domain == "RuntimeFunctionStarts":
        for rva in pe.runtime_starts:
            for sec in pe.sections:
                if rva < sec.rva:
                    continue
                off = rva - sec.rva
                if off + fp.span <= len(sec.data) and norm_ok(sec.data, off, fp, pattern, masks):
                    hits.append(rva)
                    break
        return hits
    anchor = pattern[fp.blob_offset + fp.anchor_offset : fp.blob_offset + fp.anchor_offset + fp.anchor_length]
    for sec in pe.sections:
        data = sec.data
        if len(data) < fp.span:
            continue
        start = 0
        while True:
            off_anchor = data.find(anchor, start)
            if off_anchor < 0:
                break
            off = off_anchor - fp.anchor_offset
            start = off_anchor + 1
            if off < 0 or off + fp.span > len(data):
                continue
            if norm_ok(data, off, fp, pattern, masks):
                hits.append(sec.rva + off)
    return hits


def at(pe: PeImage, rva: int, n: int) -> bytes | None:
    if rva < 0 or rva + n > pe.size:
        return None
    return bytes(pe.image[rva : rva + n])


def find_bytes(pe: PeImage, needle: bytes, mask: bytes | None = None) -> list[int]:
    hits = []
    n = len(needle)
    for sec in pe.sections:
        data = sec.data
        if len(data) < n:
            continue
        if mask is None:
            start = 0
            while True:
                off = data.find(needle, start)
                if off < 0:
                    break
                hits.append(sec.rva + off)
                start = off + 1
        else:
            for off in range(len(data) - n + 1):
                ok = True
                for i in range(n):
                    if (data[off + i] & mask[i]) != (needle[i] & mask[i]):
                        ok = False
                        break
                if ok:
                    hits.append(sec.rva + off)
    return hits


def main() -> None:
    exact = load_exact(SRC / "kirkware_hook_fingerprints.hpp")
    norm, pattern, masks = load_normalized(SRC / "kirkware_hook_normalized.hpp")
    hooks = load_minhooks(SRC / "install_kirkware_game_hooks.cpp")
    pes = {
        "Client": load_pe(GMOD / "client.dll"),
        "Engine": load_pe(GMOD / "engine.dll"),
        "LuaShared": load_pe(GMOD / "lua_shared.dll"),
        "Vgui2": load_pe(GMOD / "vgui2.dll"),
        "Studio": load_pe(GMOD / "studiorender.dll"),
    }
    print("PE")
    for name, pe in pes.items():
        print(f"  {name:10} size=0x{pe.size:X} exec={len(pe.sections)} rf={len(pe.runtime_starts)} {pe.path.name}")
        for sec in pe.sections:
            print(f"    exec rva=0x{sec.rva:X} size=0x{len(sec.data):X}")

    print("\nMINHOOKS")
    hard = []
    moved = []
    exact_miss = []
    ambig = []
    resolved = []
    for i, (h, e, n) in enumerate(zip(hooks, exact, norm)):
        pe = pes[h.module]
        base_bytes = at(pe, h.baseline, 16)
        base_hex = base_bytes.hex(" ") if base_bytes else "OOB"
        exact_hits = scan_exact(pe, e)
        if n.domain == "CurrentExactOnly":
            norm_hits: list[int] = []
        else:
            norm_hits = scan_norm(pe, n, pattern, masks)
        exact_state = (
            "unique" if len(exact_hits) == 1 else ("miss" if not exact_hits else f"ambig:{len(exact_hits)}")
        )
        if n.domain == "CurrentExactOnly":
            norm_state = "exact_only"
        else:
            norm_state = (
                "unique" if len(norm_hits) == 1 else ("miss" if not norm_hits else f"ambig:{len(norm_hits)}")
            )
        if exact_hits:
            new_rva = exact_hits[0] if len(exact_hits) == 1 else None
        elif len(norm_hits) == 1:
            new_rva = norm_hits[0]
        else:
            new_rva = None
        delta = None
        if new_rva is not None:
            delta = new_rva - h.baseline
            resolved.append((i, h.module, new_rva, "exact" if len(exact_hits) == 1 else "norm"))
        status = "OK"
        if new_rva is None:
            status = "FAIL"
            hard.append(i)
        elif len(exact_hits) > 1:
            status = "AMBIG"
            ambig.append(i)
        elif not exact_hits:
            status = "FALLBACK"
            exact_miss.append(i)
        if new_rva is not None and new_rva != h.baseline:
            moved.append((i, h.baseline, new_rva, delta))
        print(
            f"  {i:02d} {h.module:10} base=0x{h.baseline:06X} "
            f"exact={exact_state:10} norm={norm_state:10} "
            f"new={('0x%06X' % new_rva) if new_rva is not None else '----':>10} "
            f"d={delta if delta is not None else '-':>7} {status}  {base_hex}"
        )
        if status == "FAIL":
            print(f"      prefix={e.prefix.hex(' ')} offs={list(e.offsets)} vals={e.values.hex(' ')}")

    print("\nSUMMARY")
    print(f"  fail={hard} fallback={exact_miss} ambig={ambig} moved={len(moved)}")
    if resolved:
        print("  new RVAs:")
        line = []
        for i, mod, rva, how in resolved:
            line.append(f"{i:02d}:{how[0]}:{rva:05X}")
        print("   " + " ".join(line))

    # uniqueness across resolved
    by_mod: dict[str, dict[int, list[int]]] = {}
    for i, mod, rva, how in resolved:
        by_mod.setdefault(mod, {}).setdefault(rva, []).append(i)
    for mod, mapping in by_mod.items():
        dups = {rva: idxs for rva, idxs in mapping.items() if len(idxs) > 1}
        if dups:
            print(f"  DUP {mod}: {dups}")

    print("\nDIRECT PREFIXES")
    client = pes["Client"]
    engine = pes["Engine"]
    lua = pes["LuaShared"]
    vgui = pes["Vgui2"]
    studio = pes["Studio"]

    fsn = bytes([0x48, 0x83, 0xEC, 0x28, 0x89, 0x15])
    fsn_hits = []
    for rva in find_bytes(client, fsn):
        b = at(client, rva, 13)
        if b and b[10:13] == bytes([0x83, 0xFA, 0x06]):
            fsn_hits.append(rva)
    print(f"  FSN vt35-like cmp edx,6: {['0x%X' % x for x in fsn_hits[:8]]} count={len(fsn_hits)}")

    dme = bytes([0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57])
    dme_hits = find_bytes(engine, dme)
    print(f"  DME 12-byte prefix: {['0x%X' % x for x in dme_hits[:8]]} count={len(dme_hits)}")

    lua4 = bytes([0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xDA])
    lua5 = bytes([0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02])
    print(f"  Lua vt4: {['0x%X' % x for x in find_bytes(lua, lua4)]}")
    print(f"  Lua vt5: {['0x%X' % x for x in find_bytes(lua, lua5)]}")

    vgui_p = bytes([0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01])
    print(f"  Vgui prefix: count={len(find_bytes(vgui, vgui_p))} first={['0x%X' % x for x in find_bytes(vgui, vgui_p)[:6]]}")

    runstring = bytes(
        [
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
            0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50,
            0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01,
            0x00, 0x00, 0x48, 0x8B, 0x05,
        ]
    )
    print(f"  Lua RunString prefix: {['0x%X' % x for x in find_bytes(lua, runstring)]}")

    mr = bytes(
        [
            0x48, 0x8B, 0xCF, 0xF3, 0x0F, 0x11, 0x44, 0x24,
            0x28, 0x44, 0x89, 0x6C, 0x24, 0x20, 0xE8, 0x63,
            0x01, 0x00, 0x00, 0x8B, 0x8C, 0x24, 0xC0, 0x00,
            0x00, 0x00, 0x03, 0xC8,
        ]
    )
    print(f"  Studio model-render call fp: {['0x%X' % x for x in find_bytes(studio, mr)]}")
    callee = bytes(
        [
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC,
            0x24, 0xA8, 0xFD, 0xFF, 0xFF,
        ]
    )
    print(f"  Studio callee prefix: {['0x%X' % x for x in find_bytes(studio, callee)]}")

    # baseline original RVAs from DirectHookSpec
    print("\nDIRECT BASELINE BYTES")
    for label, pe, rva in [
        ("FSN 0x203440", client, 0x203440),
        ("Eng95 0x797A0", engine, 0x797A0),
        ("DME 0xFEF90", engine, 0xFEF90),
        ("Lua4 0x134A0", lua, 0x134A0),
        ("Lua5 0x131A0", lua, 0x131A0),
        ("Vgui 0x1C880", vgui, 0x1C880),
    ]:
        b = at(pe, rva, 16)
        print(f"  {label}: {b.hex(' ') if b else 'OOB'}")


if __name__ == "__main__":
    main()
