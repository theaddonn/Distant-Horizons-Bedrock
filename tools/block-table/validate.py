#!/usr/bin/env python3
"""Validate data/block_table.json (and biome_table.json) against the pack's blocks.json.

Usage: python3 tools/block-table/validate.py [--samples DIR] [--data DIR]

Checks: every block id from blocks.json (and the vanilla block list, when --samples
is given) has an entry; material is a known non-null BlockMaterial name; opacity in
0..16; lightEmission in 0..15; colours parse as #RRGGBBAA; tint is a known kind.
Exit status is non-zero on any failure.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jsonc  # noqa: E402

MATERIALS = {"Unknown", "Leaves", "Stone", "Wood", "Metal", "Dirt", "Lava", "DeepSlate",
             "Snow", "Sand", "Terracotta", "NetherStone", "Water", "Grass", "Air", "Illuminated"}
TINTS = {"none", "grass", "foliage", "dry_foliage", "birch", "evergreen", "water"}
RGBA = re.compile(r"^#[0-9A-Fa-f]{8}$")
RGB = re.compile(r"^#[0-9A-Fa-f]{6}$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "..", "..", "data"))
    ap.add_argument("--samples", default=os.environ.get("BEDROCK_SAMPLES"))
    args = ap.parse_args()

    errors = []
    with open(os.path.join(args.data, "block_table.json"), encoding="utf-8") as f:
        table = json.load(f)
    blocks = table["blocks"]
    if table.get("version") != 1:
        errors.append("block_table version != 1")

    expected = set()
    if args.samples:
        bj = jsonc.load(os.path.join(args.samples, "resource_pack", "blocks.json"))
        expected |= {"minecraft:" + k for k in bj if k != "format_version"}
        with open(os.path.join(args.samples, "metadata", "vanilladata_modules", "mojang-blocks.json"),
                  encoding="utf-8") as f:
            expected |= {b["name"] for b in json.load(f)["data_items"]}
    for bid in sorted(expected - set(blocks)):
        errors.append("missing entry: " + bid)

    srcs, mats = {}, {}
    for bid, b in blocks.items():
        if not bid.startswith("minecraft:"):
            errors.append("%s: missing minecraft: prefix" % bid)
        if b.get("material") not in MATERIALS:
            errors.append("%s: bad material %r" % (bid, b.get("material")))
        if not (isinstance(b.get("opacity"), int) and 0 <= b["opacity"] <= 16):
            errors.append("%s: opacity out of range %r" % (bid, b.get("opacity")))
        if not (isinstance(b.get("lightEmission"), int) and 0 <= b["lightEmission"] <= 15):
            errors.append("%s: lightEmission out of range %r" % (bid, b.get("lightEmission")))
        for k in ("isAir", "isSolid", "isLiquid", "isIce"):
            if not isinstance(b.get(k), bool):
                errors.append("%s: %s not bool" % (bid, k))
        if not RGBA.match(str(b.get("color"))):
            errors.append("%s: bad color %r" % (bid, b.get("color")))
        fc = b.get("faceColors") or {}
        for face in ("up", "side", "down"):
            if not RGBA.match(str(fc.get(face))):
                errors.append("%s: bad faceColors.%s %r" % (bid, face, fc.get(face)))
        if b.get("tint") not in TINTS:
            errors.append("%s: bad tint %r" % (bid, b.get("tint")))
        if b.get("isAir") and b.get("material") != "Air":
            errors.append("%s: isAir but material %s" % (bid, b.get("material")))
        srcs[b.get("colorSource")] = srcs.get(b.get("colorSource"), 0) + 1
        mats[b.get("material")] = mats.get(b.get("material"), 0) + 1

    # biome table
    with open(os.path.join(args.data, "biome_table.json"), encoding="utf-8") as f:
        bt = json.load(f)
    nulls = 0
    for name, b in bt["biomes"].items():
        if not RGB.match(str(b.get("waterColor"))):
            errors.append("%s: bad waterColor %r" % (name, b.get("waterColor")))
        for k in ("grassTint", "foliageTint", "birchTint", "evergreenTint", "dryFoliageTint"):
            v = b.get(k)
            if v is None:
                nulls += 1
            elif not RGB.match(str(v)):
                errors.append("%s: bad %s %r" % (name, k, v))

    print("blocks in table      : %d (%d legacy ids)" % (len(blocks), sum(1 for b in blocks.values() if b.get("legacy"))))
    print("expected ids checked : %d" % len(expected))
    print("colour sources       :", dict(sorted(srcs.items())))
    print("materials            :", dict(sorted(mats.items())))
    print("fallback to default colour: %d   fallback material Unknown: %d"
          % (srcs.get("default", 0), mats.get("Unknown", 0)))
    print("biomes               : %d (%d null tint fields)" % (len(bt["biomes"]), nulls))
    if errors:
        print("\nFAILED with %d error(s):" % len(errors))
        for e in errors[:50]:
            print("  " + e)
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
