#!/usr/bin/env python3
"""Generate data/block_table.json and data/biome_table.json for the DH Bedrock port.

Usage:
    python3 tools/block-table/generate.py --samples /path/to/bedrock-samples [--out data]

See README.md for the input layout and the heuristics used.
"""
import argparse
import fnmatch
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jsonc  # noqa: E402

from PIL import Image  # noqa: E402

GAME_VERSION = "1.26.51"
FLOWER_COLOR_SCALE = 5  # Java ClientBlockStateColorCache.FLOWER_COLOR_SCALE

MATERIALS = ["Unknown", "Leaves", "Stone", "Wood", "Metal", "Dirt", "Lava", "DeepSlate",
             "Snow", "Sand", "Terracotta", "NetherStone", "Water", "Grass", "Air", "Illuminated"]


# --------------------------------------------------------------------------- helpers

def glob_any(name, patterns):
    return any(fnmatch.fnmatchcase(name, p) for p in patterns)


def hexcolor(r, g, b, a=255):
    return "#%02X%02X%02X%02X" % (r, g, b, a)


def hex_rgb(r, g, b):
    return "#%02X%02X%02X" % (r, g, b)


def parse_hex(s):
    s = s.lstrip("#")
    if len(s) == 6:
        s += "FF"
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4, 6))


def srgb_to_linear(c8):
    c = c8 / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb8(v):
    v = min(max(v, 0.0), 1.0)
    c = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return min(255, max(0, int(round(c * 255.0))))


SRGB_LIN = [srgb_to_linear(i) for i in range(256)]


# --------------------------------------------------------------------------- texture averaging

class TextureCache:
    def __init__(self, pack_dir):
        self.pack = pack_dir
        self.cache = {}

    def _open(self, path):
        for ext in (".png", ".tga"):
            p = os.path.join(self.pack, path + ext)
            if os.path.isfile(p):
                return Image.open(p).convert("RGBA")
        return None

    def average(self, path, mode):
        """Port of ClientBlockStateColorCache.calculateColorFromTexture. Returns (r,g,b,a) or None if missing file."""
        key = (path, mode)
        if key in self.cache:
            return self.cache[key]
        img = self._open(path)
        if img is None:
            self.cache[key] = None
            return None
        w, h = img.size
        if h > w:  # flipbook strip: only the first frame is used (as Java does)
            img = img.crop((0, 0, w, w))
        count = 0
        alpha = 0
        red = green = blue = 0.0
        for r, g, b, a in img.getdata():
            scale = 1
            if mode == "leaves":
                if a == 0:
                    continue
                a = 255
            elif a == 0 and mode != "glass":
                continue
            elif mode == "flower" and (g + 25 < b or g + 25 < r):
                scale = FLOWER_COLOR_SCALE
            count += scale
            alpha += a * scale
            red += SRGB_LIN[r] * a * scale
            green += SRGB_LIN[g] * a * scale
            blue += SRGB_LIN[b] * a * scale
        if count == 0 or alpha == 0:
            res = (255, 255, 255, 0)
        else:
            res = (linear_to_srgb8(red / alpha), linear_to_srgb8(green / alpha),
                   linear_to_srgb8(blue / alpha), alpha // count)
        if res == (182, 0, 182, 255):  # missing-texture magenta -> do not render
            res = (255, 255, 255, 0)
        self.cache[key] = res
        return res


# --------------------------------------------------------------------------- block table

class Ctx:
    pass


def load_inputs(samples):
    c = Ctx()
    c.rp = os.path.join(samples, "resource_pack")
    c.bp = os.path.join(samples, "behavior_pack")
    c.blocks_json = jsonc.load(os.path.join(c.rp, "blocks.json"))
    c.terrain = jsonc.load(os.path.join(c.rp, "textures", "terrain_texture.json"))["texture_data"]
    with open(os.path.join(samples, "metadata", "vanilladata_modules", "mojang-blocks.json"), encoding="utf-8") as f:
        c.block_ids = [b["name"] for b in json.load(f)["data_items"]]
    c.ov = jsonc.load(os.path.join(HERE, "overrides.json"))
    c.tex = TextureCache(c.rp)
    return c


def texture_file(c, tex_name):
    e = c.terrain.get(tex_name)
    if e is None:
        return None
    t = e.get("textures")
    if isinstance(t, list):
        t = t[0] if t else None
    if isinstance(t, dict):
        t = t.get("path")
    return t


def faces_of(entry):
    """Expand a blocks.json 'textures' value to {up,down,north,east,south,west} texture names."""
    t = entry.get("textures") if isinstance(entry, dict) else None
    if t is None:
        return None
    if isinstance(t, str):
        return {f: t for f in ("up", "down", "north", "east", "south", "west")}
    side = t.get("side")
    out = {}
    for f in ("north", "east", "south", "west"):
        out[f] = t.get(f) or side
    out["up"] = t.get("up") or side
    out["down"] = t.get("down") or side
    if any(v is None for v in out.values()):
        first = next((v for v in t.values() if isinstance(v, str)), None)
        for k, v in out.items():
            if v is None:
                out[k] = first
    return out


def find_entry(c, name):
    """Return (entry, source) from blocks.json, following aliases for ids it lacks."""
    e = c.blocks_json.get(name)
    if isinstance(e, dict) and "textures" in e:
        return e, "texture"
    alias = c.ov["aliases"].get(name)
    if alias is None:
        for pre in c.ov["aliasPrefixStrip"]:
            if name.startswith(pre):
                alias = name[len(pre):]
    if alias:
        e = c.blocks_json.get(alias)
        if isinstance(e, dict) and "textures" in e:
            return e, "alias"
        if alias != name:
            return find_entry(c, alias)
    return None, None


def classify_tint(c, name):
    for kind, pats in c.ov["tint"].items():
        if kind.startswith("_"):
            continue
        if glob_any(name, pats):
            return kind
    return "none"


def sound_of(c, name):
    e, _ = find_entry(c, name)
    if e is None:
        return None
    return e.get("sound")


def classify_material(c, name, sound, light, is_air, is_liquid):
    ov = c.ov
    if name in ov["materialOverrides"] and not name.startswith("_"):
        return ov["materialOverrides"][name]
    if is_air:
        return "Air"
    if "leaves" in name or any(k in name for k in ("bamboo", "cactus", "chorus_flower", "mushroom")):
        return "Leaves"
    if name in ("lava", "flowing_lava"):
        return "Lava"
    if is_liquid or name in ("water", "flowing_water"):
        return "Water"
    if sound in ov["woodSounds"] or "root" in name:
        return "Wood"
    if sound in ov["metalSounds"]:
        return "Metal"
    if "grass_block" in name or "grass_slab" in name:
        return "Grass"
    if any(k in name for k in ("dirt", "gravel", "mud", "podzol", "mycelium")):
        return "Dirt"
    if sound in ov["deepslateSounds"] or "deepslate" in name:
        return "DeepSlate"
    if name in ("netherrack", "basalt", "blackstone"):
        return "NetherStone"
    if "snow" in name:
        return "Snow"
    if "sand" in name:
        return "Sand"
    if "terracotta" in name or "hardened_clay" in name:
        return "Terracotta"
    if "stone" in name or "ore" in name:
        return "Stone"
    if light > 0:
        return "Illuminated"
    return "Unknown"


def light_of(c, name):
    for pat, lvl in c.ov["light"].items():
        if pat.startswith("_"):
            continue
        if fnmatch.fnmatchcase(name, pat):
            return lvl
    return 0


def build_block(c, bid, legacy):
    ov = c.ov
    name = bid[len("minecraft:"):]
    is_air = name == "air"
    is_liquid = name in ov["liquids"]
    is_ice = name in ov["ice"]
    light = light_of(c, name)
    entry, src = find_entry(c, name)
    sound = entry.get("sound") if entry else None

    non_solid = glob_any(name, ov["nonSolid"])
    if is_air or is_liquid:
        opacity = 0 if is_air else 1
    elif glob_any(name, ov["opacityZero"]) or (non_solid and name != "snow_layer"):
        opacity = 0
    else:
        opacity = 16
    is_solid = not (is_air or non_solid)

    material = classify_material(c, name, sound, light, is_air, is_liquid)

    # ----- colour
    tint = classify_tint(c, name)
    mode = "default"
    if "leaves" in name:
        mode = "leaves"
    elif name in ov["flowers"]:
        mode = "flower"
    elif "glass" in name:
        mode = "glass"

    color = None
    face_colors = None
    color_source = None
    if glob_any(name, ov["invisible"]):
        color, face_colors, color_source = (255, 255, 255, 0), None, "invisible"
    elif entry is not None:
        faces = faces_of(entry)
        res = {}
        if faces:
            for f, tn in faces.items():
                path = texture_file(c, tn) if tn else None
                avg = c.tex.average(path, mode) if path else None
                if avg is not None:
                    res[f] = avg
        if res:
            # Java face order UP, N, E, W, S, DOWN; pillars (logs) skip UP so the bark colour is used.
            order = ["up", "north", "east", "west", "south", "down"]
            if glob_any(name, ov["pillars"]):
                order = order[1:] + ["up"]
            pick = next((f for f in order if f in res), None)
            color = res[pick]
            side = res.get("north") or res.get("east") or res.get("south") or res.get("west") or color
            face_colors = {"up": res.get("up", side), "side": side, "down": res.get("down", side)}
            color_source = src
    if color is None:
        if name in ov["colors"] and not name.startswith("_"):
            color = parse_hex(ov["colors"][name])
            color_source = "override"
        else:
            for pat, col in ov["colorGlobs"].items():
                if not pat.startswith("_") and fnmatch.fnmatchcase(name, pat):
                    color = parse_hex(col)
                    color_source = "override"
                    break
    if color is None:
        color = (128, 128, 128, 255)
        color_source = "default"
    if face_colors is None:
        face_colors = {"up": color, "side": color, "down": color}

    out = {
        "material": material,
        "opacity": opacity,
        "lightEmission": light,
        "isAir": is_air,
        "isSolid": is_solid,
        "isLiquid": is_liquid,
        "isIce": is_ice,
        "color": hexcolor(*color),
        "faceColors": {k: hexcolor(*v) for k, v in face_colors.items()},
        "tint": tint,
    }
    if tint != "none":
        out["tintFaces"] = ov["tintFaces"].get(name, ["up", "side", "down"])
    out["colorSource"] = color_source
    if legacy:
        out["legacy"] = True
    return out


def build_block_table(c):
    ids = list(c.block_ids)
    known = set(ids)
    legacy = set()
    for k in c.blocks_json:
        if k == "format_version":
            continue
        bid = "minecraft:" + k
        if bid not in known:
            ids.append(bid)
            legacy.add(bid)
    blocks = {}
    for bid in sorted(set(ids)):
        blocks[bid] = build_block(c, bid, bid in legacy)
    return blocks


# --------------------------------------------------------------------------- biome table

def load_colormap(c, name):
    p = os.path.join(c.rp, "textures", "colormap", name + ".png")
    return Image.open(p).convert("RGB")


def sample_colormap(img, temp, downfall):
    """Vanilla colormap lookup (identical in Java and Bedrock)."""
    t = min(max(temp, 0.0), 1.0)
    d = min(max(downfall, 0.0), 1.0) * t
    w, h = img.size
    x = int((1.0 - t) * (w - 1))
    y = int((1.0 - d) * (h - 1))
    return img.getpixel((x, y))


def build_biome_table(c, samples):
    rp, bp = c.rp, c.bp
    legacy = jsonc.load(os.path.join(rp, "biomes_client.json"))["biomes"]
    default = legacy["default"]
    with open(os.path.join(samples, "metadata", "vanilladata_modules", "mojang-biomes.json"), encoding="utf-8") as f:
        ids = {b["name"]: b["id"] for b in json.load(f)["data_items"]}

    cmaps = {n: load_colormap(c, n) for n in
             ("grass", "foliage", "birch", "evergreen", "dry_foliage",
              "swamp_grass", "swamp_foliage", "mangrove_swamp_foliage")}

    names = set(ids)
    bdir = os.path.join(bp, "biomes")
    for fn in os.listdir(bdir):
        if fn.endswith(".biome.json"):
            names.add("minecraft:" + fn[:-len(".biome.json")])

    biomes = {}
    stats = {"grass_null": 0}
    for name in sorted(names):
        short = name[len("minecraft:"):]
        beh_p = os.path.join(bdir, short + ".biome.json")
        cli_p = os.path.join(rp, "biomes", short + ".client_biome.json")
        comp = {}
        if os.path.isfile(beh_p):
            comp = jsonc.load(beh_p)["minecraft:biome"]["components"]
        ccomp = {}
        if os.path.isfile(cli_p):
            ccomp = jsonc.load(cli_p)["minecraft:client_biome"]["components"]
        legacy_b = legacy.get(name, {})

        climate = comp.get("minecraft:climate")
        temp = climate.get("temperature") if climate else None
        down = climate.get("downfall") if climate else None

        water = ccomp.get("minecraft:water_appearance", {})
        water_color = water.get("surface_color") or legacy_b.get("water_surface_color") or default["water_surface_color"]
        water_opacity = water.get("surface_opacity",
                                  legacy_b.get("water_surface_transparency", default["water_surface_transparency"]))
        fog = ccomp.get("minecraft:fog_appearance", {}).get("fog_identifier") \
            or legacy_b.get("fog_identifier") or default.get("fog_identifier")

        def tint_for(comp_key, legacy_key, cmap_name, swamp_cmap_names):
            spec = ccomp.get(comp_key, {}).get("color")
            if isinstance(spec, str):
                return spec.upper(), "fixed"
            if isinstance(spec, dict) and spec.get("color_map") in cmaps:
                if temp is None:
                    return None, "colormap:" + spec["color_map"]
                r, g, b = sample_colormap(cmaps[spec["color_map"]], temp, down)
                return hex_rgb(r, g, b), "colormap:" + spec["color_map"]
            if temp is None:
                return None, "unknown"
            r, g, b = sample_colormap(cmaps[cmap_name], temp, down)
            return hex_rgb(r, g, b), "colormap:" + cmap_name

        grass, grass_mode = tint_for("minecraft:grass_appearance", None, "grass", None)
        foliage, foliage_mode = tint_for("minecraft:foliage_appearance", None, "foliage", None)
        if temp is not None:
            birch = hex_rgb(*sample_colormap(cmaps["birch"], temp, down))
            evergreen = hex_rgb(*sample_colormap(cmaps["evergreen"], temp, down))
        else:
            birch = evergreen = None
        dry = ccomp.get("minecraft:dry_foliage_color", {}).get("color")
        if isinstance(dry, str):
            dry_foliage = dry.upper()
        elif temp is not None:
            dry_foliage = hex_rgb(*sample_colormap(cmaps["dry_foliage"], temp, down))
        else:
            dry_foliage = None
        if grass is None:
            stats["grass_null"] += 1

        biomes[name] = {
            "id": ids.get(name),
            "temperature": temp,
            "downfall": down,
            "waterColor": water_color.upper(),
            "waterOpacity": water_opacity,
            "fogId": fog,
            "grassTint": grass,
            "grassTintMode": grass_mode,
            "foliageTint": foliage,
            "foliageTintMode": foliage_mode,
            "birchTint": birch,
            "evergreenTint": evergreen,
            "dryFoliageTint": dry_foliage,
        }
    return {
        "default": {"waterColor": default["water_surface_color"].upper(),
                    "waterOpacity": default["water_surface_transparency"],
                    "fogId": default.get("fog_identifier")},
        "biomes": biomes,
    }, stats


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--samples", default=os.environ.get("BEDROCK_SAMPLES"),
                    help="path to a checkout of github.com/Mojang/bedrock-samples")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "data"))
    ap.add_argument("--samples-version", default="v1.26.50.4",
                    help="tag of the bedrock-samples checkout (recorded in the output)")
    args = ap.parse_args()
    if not args.samples:
        ap.error("--samples (or $BEDROCK_SAMPLES) is required")

    c = load_inputs(args.samples)
    source = {"pack": "bedrock-samples %s resource_pack" % args.samples_version,
              "gameVersion": GAME_VERSION}

    blocks = build_block_table(c)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "block_table.json"), "w", encoding="utf-8") as f:
        json.dump({"version": 1, "source": source, "blocks": blocks}, f, indent=1, sort_keys=False)
        f.write("\n")

    biome, bstats = build_biome_table(c, args.samples)
    with open(os.path.join(args.out, "biome_table.json"), "w", encoding="utf-8") as f:
        json.dump({"version": 1, "source": source, **biome}, f, indent=1)
        f.write("\n")

    srcs = {}
    mats = {}
    for b in blocks.values():
        srcs[b["colorSource"]] = srcs.get(b["colorSource"], 0) + 1
        mats[b["material"]] = mats.get(b["material"], 0) + 1
    print("blocks: %d  biomes: %d" % (len(blocks), len(biome["biomes"])))
    print("colour sources:", dict(sorted(srcs.items())))
    print("materials:", dict(sorted(mats.items())))
    print("biomes with null grass tint:", bstats["grass_null"])


if __name__ == "__main__":
    main()
