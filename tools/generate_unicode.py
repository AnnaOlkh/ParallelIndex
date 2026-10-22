from pathlib import Path
import unicodedata as ud

ranges = []
start = previous = None
for cp in range(0x110000):
    cat = ud.category(chr(cp))
    if cat.startswith("L") or cat == "Nd":
        if start is None:
            start = cp
        previous = cp
    elif start is not None:
        ranges.append((start, previous))
        start = previous = None
if start is not None:
    ranges.append((start, previous))
lines = ["#pragma once", "#include <cstdint>", "#include <string_view>",
         "namespace lab::unicode {",
         f'inline constexpr char version[] = "{ud.unidata_version}";',
         "struct Range { char32_t first, last; };",
         "inline constexpr Range alnum[] = {"]
lines += [f"  {{0x{a:X}, 0x{b:X}}}," for a, b in ranges]
lines += ["};", "struct Lower { char32_t code; std::u32string_view text; };",
          "inline constexpr Lower lowercase[] = {"]
for cp in range(0x110000):
    source = chr(cp)
    lower = source.lower()
    if len(lower) != 1:
        if cp != 0x0130:
            raise ValueError(f"Review simple lowercase mapping for U+{cp:04X}")
        lower = "i"
    if lower != source:
        escaped = "".join(f"\\U{ord(ch):08X}" for ch in lower)
        lines.append(f'  {{0x{cp:X}, U"{escaped}"}},')
lines += ["};", "}", ""]
target = Path(__file__).resolve().parents[1] / "src" / "unicode_tables.hpp"
target.write_text("\n".join(lines), encoding="utf-8")
print(f"Generated {target.name}, UCD {ud.unidata_version}")
