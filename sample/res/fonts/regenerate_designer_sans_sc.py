#!/usr/bin/env python3
"""Build or check the UI Designer's Chinese fallback for UI and document text."""

import argparse
import hashlib
import io
import json
import unicodedata
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

FONTS = Path(__file__).resolve().parent
CATALOG = FONTS.parent / "ui-designer" / "zh-CN.json"
PRIMARY = FONTS / "DejaVuSans.ttf"
OUTPUT = FONTS / "DesignerSansSC.ttf"
SOURCE_SHA256 = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
SOURCE_CODEPOINTS = 30_890  # Unicode cmap size of the pinned font.
FAMILY = "Designer Sans SC"
POSTSCRIPT = "DesignerSansSC-Regular"
# Include text outside the UI catalog: common Simplified, Traditional, and
# Extension A ideographs. A catalog-only font used to drop these in documents.
DOCUMENT_TEXT = "鞋子裙智能測試㐀"


def codepoints(paths):
    chars = set(DOCUMENT_TEXT)
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
        if needed - actual:
            raise SystemExit(f"Fallback cmap lacks {describe(needed - actual)}")
        if len(actual) != SOURCE_CODEPOINTS:
            raise SystemExit(f"Expected {SOURCE_CODEPOINTS:,} code points, found {len(actual):,}")
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
        required_coverage = set(font.getBestCmap())
        retained_ids = [0, 5, 13, 14]  # Preserve copyright, version and OFL metadata.
        instantiateVariableFont(font, {"wght": 400}, inplace=True, static=True)
        if set(font.getBestCmap()) != required_coverage:
            raise SystemExit("Building the static font changed its Unicode coverage")
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
          f"{SOURCE_CODEPOINTS:,} code points ({len(requested)} catalog and test code points verified across both fonts)")


if __name__ == "__main__":
    main()
