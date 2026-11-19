# Designer Sans SC

`DesignerSansSC.ttf` is the UI Designer's Chinese fallback after `DejaVuSans.ttf`.
The workbench (including editable property values) and the live artboard use the
same font chain, regardless of the editor language. The fallback retains all
30,890 Unicode mappings in the pinned source, including characters typed into
documents that never appear in the editor's language catalogs.

This is a renamed, static Regular (400) derivative of Noto Sans SC
from [Google Fonts, pinned commit `a85815a`](https://github.com/google/fonts/tree/a85815a42757630ce188fdad368c2dfc444d4773/ofl/notosanssc).
The original copyright notice and SIL Open Font License 1.1 are in
[`LICENSE-DesignerSansSC.txt`](LICENSE-DesignerSansSC.txt). Keep that license
with the font when redistributing it. The derivative name does not use the
upstream Reserved Font Name `Source`.

## Build and verify the bundled font

Run from the repository root with Python 3 and curl (Bash syntax):

```sh
mkdir -p out/designer-font
python -m pip install --no-deps --target out/designer-font/deps fonttools==4.61.1
base=https://raw.githubusercontent.com/google/fonts/a85815a42757630ce188fdad368c2dfc444d4773/ofl/notosanssc
curl -fsSL "$base/NotoSansSC%5Bwght%5D.ttf" -o out/designer-font/NotoSansSC.ttf
export PYTHONPATH=out/designer-font/deps
python sample/res/fonts/regenerate_designer_sans_sc.py --source out/designer-font/NotoSansSC.ttf
python sample/res/fonts/regenerate_designer_sans_sc.py --check
```

The regeneration script checks the pinned input's SHA-256 before building.
`--check` only requires the checked-in fonts and catalog, not the upstream file.
