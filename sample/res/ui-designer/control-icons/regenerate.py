#!/usr/bin/env python3
"""Regenerate control-palette.rgba from Twemoji (requires Pillow and curl)."""

from concurrent.futures import ThreadPoolExecutor
from io import BytesIO
from pathlib import Path
from subprocess import check_output

from PIL import Image

BASE = "https://raw.githubusercontent.com/jdecked/twemoji/v15.0.3/assets/72x72/"
SIZE = 24
# Keep this order in sync with control_icon_types in ui_designer_workbench.cpp.
CODEPOINTS = (
    "1f4e6", "1f3f7", "1f532", "2328", "1f5bc", "1f308", "2611", "1f518", "1f51b",
    "1f39a", "1f4ca", "2796", "1f4cb", "1f333", "1f4dc", "1f5c2", "2194", "1f4d1", "1f9e9",
)


def pixels(codepoint):
    data = check_output(["curl", "--fail", "--silent", "--show-error", "--location", "--max-time", "20", f"{BASE}{codepoint}.png"])
    with Image.open(BytesIO(data)) as image:
        tile = image.convert("RGBA").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    # The original ON arrow and minus are near black on the dark palette.
    if codepoint in {"1f51b", "2796"}:
        alpha = tile.getchannel("A")
        tile = Image.new("RGBA", tile.size, "#5686B6")
        tile.putalpha(alpha)
    return tile.tobytes()


if __name__ == "__main__":
    with ThreadPoolExecutor(max_workers=6) as pool:
        atlas = b"".join(pool.map(pixels, CODEPOINTS))
    Path(__file__).with_name("control-palette.rgba").write_bytes(atlas)
