#!/usr/bin/env python3
"""Embed Kirkware client Lua files into a generated C++ header.

The native bridge loader itself is excluded because its only purpose is to load
this binary module; embedding it would create a redundant require() cycle.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

EXCLUDED = {"kirkware_linux_native.lua"}


def byte_lines(data: bytes, width: int = 16) -> list[str]:
    lines: list[str] = []
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    if not data:
        lines.append("    0x00,")
    return lines


def render(source: Path) -> str:
    files = sorted(
        path for path in source.glob("*.lua")
        if path.is_file() and path.name not in EXCLUDED
    )
    if not files:
        raise SystemExit(f"no client Lua payloads found in {source}")

    out: list[str] = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        "namespace KirkwareEmbedded {",
        "struct LuaPayload {",
        "    const char *name;",
        "    const unsigned char *data;",
        "    std::size_t size;",
        "};",
        "",
    ]

    for index, path in enumerate(files):
        data = path.read_bytes()
        out.append(f"static constexpr unsigned char kPayload{index}[] = {{")
        out.extend(byte_lines(data))
        out.append("};")
        out.append("")

    out.append("static constexpr LuaPayload kPayloads[] = {")
    for index, path in enumerate(files):
        name = json.dumps(path.name)
        size = len(path.read_bytes())
        out.append(f"    {{{name}, kPayload{index}, {size}}},")
    out.extend([
        "};",
        "",
        "static constexpr std::size_t kPayloadCount =",
        "    sizeof(kPayloads) / sizeof(kPayloads[0]);",
        "} // namespace KirkwareEmbedded",
        "",
    ])
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source = args.source.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not source.is_dir():
        raise SystemExit(f"source directory does not exist: {source}")

    generated = render(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_file() and output.read_text(encoding="utf-8") == generated:
        return 0
    output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
