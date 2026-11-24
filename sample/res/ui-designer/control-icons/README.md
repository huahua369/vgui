# Control palette icons

The control palette uses [Twemoji 15.0.3](https://github.com/jdecked/twemoji/tree/v15.0.3) graphics by Twitter, Inc. and other contributors under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). See [LICENSE-GRAPHICS](LICENSE-GRAPHICS) for the license text.

The original 72 × 72 PNGs were resized to 24 × 24 and packed as consecutive straight-alpha RGBA8 tiles in `control-palette.rgba`. The ON arrow and minus were recolored for contrast in both themes. The editor embeds this file at build time; no downloads or system emoji fonts are needed. `regenerate.py` lists the original Unicode code points in tile order and recreates the atlas with Pillow and curl.
