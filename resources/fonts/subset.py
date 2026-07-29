"""Instance + subset Material Symbols Rounded to only the icons the app uses.
Produces two small static fonts: regular (FILL 0) and filled (FILL 1)."""
import os
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "MaterialSymbolsRounded.ttf")

UNI = [0xe9f4, 0xe875, 0xf02f, 0xeb7d, 0xf0be, 0xe896, 0xe8d8, 0xe8b8, 0xe668,
       0xe037, 0xe047, 0xe863, 0xe8d4, 0xe5d5, 0xe6b1, 0xf720, 0xef76, 0xe2c8,
       0xe617, 0xe02f, 0xe226, 0xe2c7, 0xeb8e, 0xe2cc, 0xe92e, 0xe145, 0xe89c,
       0xe5cd, 0xe15b, 0xe14d, 0xf090, 0xe88e]  # ... content_copy, download, info

def build(fill, family, out):
    f = TTFont(SRC)
    instancer.instantiateVariableFont(f, {"wght": 500, "GRAD": 0, "opsz": 24, "FILL": fill}, inplace=True)
    # rename family so Qt can tell the two apart
    for rec in f["name"].names:
        if rec.nameID in (1, 16):
            rec.string = family
        elif rec.nameID in (4, 3, 6):
            rec.string = family.replace(" ", "")
    opts = subset.Options()
    opts.layout_features = []          # codepoints only, no ligatures needed
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    opts.recalc_bounds = True
    ss = subset.Subsetter(options=opts)
    ss.populate(unicodes=UNI)
    ss.subset(f)
    f.save(out)
    print("wrote", out, os.path.getsize(out) // 1024, "KB")

build(0, "IDA MCP Symbols", os.path.join(HERE, "symbols.ttf"))
build(1, "IDA MCP Symbols Fill", os.path.join(HERE, "symbols_fill.ttf"))
