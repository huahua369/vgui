#!/usr/bin/env python3
"""Regenerate or check the UI Designer's catalog-sized Chinese fallback."""

import argparse
import hashlib
import io
import json
import unicodedata
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

FONTS = Path(__file__).resolve().parent
CATALOG = FONTS.parent / "ui-designer" / "zh-CN.json"
PRIMARY = FONTS / "DejaVuSans.ttf"
OUTPUT = FONTS / "DesignerSansSC.ttf"
SOURCE_SHA256 = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
FAMILY = "Designer Sans SC"
POSTSCRIPT = "DesignerSansSC-Regular"
PUNCTUATION = "　、。〈〉《》「」『』【】〔〕〖〗，．：；！？（）［］｛｝－—…“”‘’·～￥％＋＝／｜"


def codepoints(paths):
    chars = set(PUNCTUATION)
    for path in paths:
        catalog = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(catalog, dict) or not all(isinstance(s, str) for s in catalog.values()):
            raise SystemExit(f"{path}: expected a flat JSON object of strings")
        for text in catalog.values():
            chars.update(text)
    return {ord(char) for char in chars if unicodedata.category(char) not in {"Cc", "Cf"}}


def describe(points):
    return ", ".join(f"U+{point:04X} ({ascii(chr(point))})" for point in sorted(points))


def verify(file, needed):
    with TTFont(file) as font:
        cmap = font.getBestCmap()
        actual = set(cmap)
        if actual != needed:
            raise SystemExit(f"Fallback cmap mismatch: missing [{describe(needed - actual)}]; "
                             f"extra [{describe(actual - needed)}]")
        if any(font.getGlyphID(glyph) == 0 for glyph in cmap.values()):
            raise SystemExit("A fallback character maps to .notdef")
        if (font.sfntVersion != "\x00\x01\x00\x00" or "fvar" in font or "glyf" not in font
                or font["OS/2"].usWeightClass != 400
                or font["name"].getDebugName(1) != FAMILY
                or font["name"].getDebugName(2) != "Regular"
                or font["name"].getDebugName(6) != POSTSCRIPT):
            raise SystemExit("Expected a static Designer Sans SC Regular TrueType font")
        return font["maxp"].numGlyphs


def build(source, needed):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise SystemExit(f"{source}: SHA-256 does not match the pinned Noto Sans SC source")
    with TTFont(io.BytesIO(data), recalcTimestamp=False) as font:
        missing = needed - font.getBestCmap().keys()
        if missing:
            raise SystemExit(f"Source font lacks glyphs for {describe(missing)}")
        options = subset.Options()
        retained_ids = [0, 5, 13, 14]  # Preserve copyright, version and OFL metadata.
        options.name_IDs = retained_ids
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=sorted(needed))
        subsetter.subset(font)
        instantiateVariableFont(font, {"wght": 400}, inplace=True, static=True)
        # Don't carry variable-style names or metadata into the static derivative.
        if "STAT" in font:
            del font["STAT"]
        font["name"].names = [record for record in font["name"].names if record.nameID in retained_ids]
        names = {1: FAMILY, 2: "Regular", 3: "DesignerSansSC-Regular;2.004",
                 4: FAMILY + " Regular", 6: POSTSCRIPT}
        for name_id, name in names.items():
            font["name"].setName(name, name_id, 3, 1, 0x0409)
            font["name"].setName(name, name_id, 1, 0, 0)
        with io.BytesIO() as result:
            font.save(result)
            result.seek(0)
            verify(result, needed)
            OUTPUT.write_bytes(result.getvalue())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--source", type=Path, help="pinned Noto Sans SC variable TTF")
    mode.add_argument("--check", action="store_true", help="verify the current subset without rebuilding")
    parser.add_argument("catalogs", type=Path, nargs="*", help="JSON catalogs (default: zh-CN.json)")
    args = parser.parse_args()
    requested = codepoints(args.catalogs or [CATALOG])
    with TTFont(PRIMARY) as primary:
        needed = requested - primary.getBestCmap().keys()
    if args.source:
        build(args.source, needed)
    glyphs = verify(OUTPUT, needed)
    print(f"{OUTPUT.name}: {OUTPUT.stat().st_size:,} bytes, {glyphs} glyphs, "
          f"{len(needed)} fallback code points ({len(requested)} covered across both fonts)")


if __name__ == "__main__":
    main()
