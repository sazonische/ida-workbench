"""Instance + subset Roboto Flex / Roboto Mono to small static Latin fonts.

Downloads (variable) live next to this file:
  robotoflex_var.ttf   (google/fonts ofl/robotoflex)
  robotomono_var.ttf   (google/fonts ofl/robotomono)
Outfiles: robotoflex-<w>.ttf (family "Roboto Flex") and robotomono-<w>.ttf ("Roboto Mono").
"""
import os
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
UNICODES = "U+0020-00FF,U+0131,U+0152-0153,U+0394,U+2013-2014,U+2018-2019,U+201C-201D,U+2022,U+2026,U+00B7,U+00D7,U+2212"

def build(src, family, weight, out):
    f = TTFont(src)
    axes = {a.axisTag: a.defaultValue for a in f["fvar"].axes}
    axes["wght"] = weight
    if "opsz" in axes: axes["opsz"] = 14
    instancer.instantiateVariableFont(f, axes, inplace=True)
    # rename family + set weight class so Qt groups them as one family with weights
    sub = {100:"Thin",300:"Light",400:"Regular",500:"Medium",600:"SemiBold",700:"Bold"}.get(weight, "Regular")
    for rec in f["name"].names:
        if rec.nameID in (1, 16): rec.string = family
        elif rec.nameID in (2, 17): rec.string = sub
        elif rec.nameID == 4: rec.string = f"{family} {sub}"
        elif rec.nameID == 6: rec.string = f"{family.replace(' ','')}-{sub}"
    f["OS/2"].usWeightClass = weight
    opts = subset.Options()
    opts.layout_features = ["kern", "liga", "calt"]
    opts.name_IDs = ['*']
    opts.recalc_bounds = True
    ss = subset.Subsetter(options=opts)
    ss.populate(unicodes=subset.parse_unicodes(UNICODES))
    ss.subset(f)
    f.save(out)
    print("wrote", os.path.basename(out), os.path.getsize(out) // 1024, "KB")

for w in (400, 500, 600, 700):
    build(os.path.join(HERE, "robotoflex_var.ttf"), "Roboto Flex", w, os.path.join(HERE, f"robotoflex-{w}.ttf"))
for w in (400, 500):
    build(os.path.join(HERE, "robotomono_var.ttf"), "Roboto Mono", w, os.path.join(HERE, f"robotomono-{w}.ttf"))
