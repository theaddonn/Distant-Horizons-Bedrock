#!/usr/bin/env python3
"""matbin_inspect.py - read-only inspector for RenderDragon .material.bin (CompiledMaterialDefinition v26).

Layout (all little-endian; string = u32 len + bytes; bool = u8):
  u64 magic 0x0A11DA1A | string "RenderDragon.CompiledMaterialDefinition" | u64 version(26) | 4 bytes encryption ("NONE")
  string name | bool hasParent [string parent]
  u8 nSamplers  { string name | u16 reg | u8 access | u8 precision | bool uav | u8 type | string texFormat
                  | u32 unkInt | u8 unkByte | bool h2 [u8] | bool hasDefaultTex [string] | bool hasStr [string]
                  | bool hasCustom [string name, u32 size] }
  u16 nProps    { string name | u16 type(2=vec4,3=mat3,4=mat4,5=external) | (u32 num | bool hasData | data) }
  u16 v26extra  (always 0 observed)                               <-- NEW in v26 (MaterialBinTool does not know it)
  u16 nPasses   { string name | string platformBitset | string fallback | bool hasBlend [u16 blend]
                  | u16 nFlagDefs { string flag | u16 n | n*string allowedValues }    <-- v26: value LIST, not one value
                  | u32 v26extra2 (0 observed; maybe 2 x u16)                           <-- NEW in v26
                  | u16 nVariants { bool supported | u16 nFlags | u16 nBlobs
                       | nFlags*{string flag, string value}
                       | nBlobs*{ string stage | string platform | u8 stageEnum | u8 platformEnum
                                  | u16 nInputs { string name | u8 type | u8 attr | u8 sub | bool perInstance
                                                  | bool hasPrec [u8] | bool hasInterp [u8] }
                                  | u64 sourceHash | u32 len | bgfx shader blob } } }
  u64 magic (trailer)

Usage:
  matbin_inspect.py info    FILE.material.bin               # summary (samplers, props, passes, variants, platforms)
  matbin_inspect.py dump    FILE.material.bin [--json]      # everything incl. inputs per blob
  matbin_inspect.py extract FILE.material.bin OUTDIR [--pass P] [--platform Direct3D_SM65] [--flags k=v,k=v]
  matbin_inspect.py bgfx    BLOB                            # decode bgfx shader header of an extracted blob
"""
import json
import struct
import sys

MAGIC = 0x0A11DA1A
TYPES = ["Float", "Vec2", "Vec3", "Vec4", "Int", "Int2", "Int3", "Int4", "UInt", "UInt2", "UInt3", "UInt4", "Mat4"]
ATTRS = {(0, 0): "Position", (1, 0): "Normal", (2, 0): "Tangent", (3, 0): "Bitangent", (5, 0): "Indices",
         (6, 0): "Weights", (9, 0): "FrontFacing"}
for i in range(4):
    ATTRS[(4, i)] = "Color%d" % i
for i in range(9):
    ATTRS[(7, i)] = "TexCoord%d" % i
UTYPES = {0: 'Sampler', 1: 'End', 2: 'Vec4', 3: 'Mat3', 4: 'Mat4'}
STAGES = ["Vertex", "Fragment", "Compute", "Unknown"]
PLATFORMS = ["Direct3D_SM40", "Direct3D_SM50", "Direct3D_SM60", "Direct3D_SM65", "Direct3D_XB1", "Direct3D_XBX",
             "GLSL_120", "GLSL_430", "ESSL_100", "ESSL_300", "ESSL_310", "Metal", "Vulkan", "Nvn", "Pssl", "Unknown"]
BLEND = ["None", "Replace", "AlphaBlend", "ColorBlendAlphaAdd", "PreMultiplied", "InvertColor", "Additive",
         "AdditiveAlpha", "Multiply", "MultiplyBoth", "InverseSrcAlpha", "SrcAlpha"]
SAMPLER_TYPES = ["Type2D", "Type2DArray", "Type2DExternal", "Type3D", "TypeCube", "TypeStructuredBuffer",
                 "TypeRawBuffer", "TypeAccelerationStructure", "Type2DShadow", "Type2DArrayShadow"]
ACCESS = ["None", "Read", "Write", "ReadWrite"]
PRECISION = ["LOWP", "MEDIUMP", "HIGHP"]
INTERP = ["FLAT", "SMOOTH", "NOPERSPECTIVE", "CENTROID"]
PROP_SIZES = {2: 16, 3: 36, 4: 64}


class R:
    def __init__(self, d):
        self.d, self.p = d, 0

    def take(self, n):
        v = self.d[self.p:self.p + n]
        if len(v) != n:
            raise EOFError("short read at %d" % self.p)
        self.p += n
        return v

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def boolean(self): return self.u8() != 0
    def string(self): return self.take(self.u32()).decode("utf-8", "replace")


def parse(data):
    r = R(data)
    m = {}
    assert r.u64() == MAGIC, "bad magic"
    assert r.string() == "RenderDragon.CompiledMaterialDefinition"
    m["version"] = r.u64()
    m["encryption"] = r.take(4).decode()
    if m["version"] != 26 or m["encryption"] != "ENON":  # 'NONE' stored byte-reversed (u32 0x454E4F4E)
        raise ValueError("unsupported version/encryption: %r %r" % (m["version"], m["encryption"]))
    m["name"] = r.string()
    m["parent"] = r.string() if r.boolean() else None
    m["samplers"] = {}
    for _ in range(r.u8()):
        n = r.string()
        s = {"reg": r.u16(), "access": ACCESS[r.u8()], "precision": r.u8(), "uav": r.boolean(),
             "type": SAMPLER_TYPES[r.u8()], "format": r.string(), "unkInt": r.u32(), "unkByte": r.u8()}
        if r.boolean(): s["unkByte2"] = r.u8()
        if r.boolean(): s["defaultTexture"] = r.string()
        if r.boolean(): s["unkString"] = r.string()
        if r.boolean(): s["custom"] = (r.string(), r.u32())
        m["samplers"][n] = s
    m["props"] = {}
    for _ in range(r.u16()):
        n = r.string()
        t = r.u16()
        f = {"type": {2: "vec4", 3: "mat3", 4: "mat4", 5: "external"}.get(t, t)}
        if t in PROP_SIZES:
            f["num"] = r.u32()
            if r.boolean():
                raw = r.take(PROP_SIZES[t])
                f["default"] = list(struct.unpack("<%df" % (len(raw) // 4), raw))
        m["props"][n] = f
    m["extra1"] = r.u16()
    m["passes"] = {}
    for _ in range(r.u16()):
        pn = r.string()
        ps = {"platformBitset": r.string(), "fallback": r.string()}
        ps["blend"] = BLEND[r.u16()] if r.boolean() else None
        ps["flagValues"] = {}
        for _ in range(r.u16()):
            k = r.string()
            ps["flagValues"][k] = [r.string() for _ in range(r.u16())]
        ps["extra2"] = r.u32()
        ps["variants"] = []
        for _ in range(r.u16()):
            v = {"supported": r.boolean()}
            nf, nb = r.u16(), r.u16()
            v["flags"] = {}
            for _ in range(nf):
                k = r.string()
                v["flags"][k] = r.string()
            v["blobs"] = []
            for _ in range(nb):
                b = {"stageName": r.string(), "platformName": r.string()}
                b["stage"], b["platform"] = STAGES[r.u8()], PLATFORMS[r.u8()]
                b["inputs"] = []
                for _ in range(r.u16()):
                    i = {"name": r.string(), "type": TYPES[r.u8()]}
                    a = (r.u8(), r.u8())
                    i["semantic"] = ATTRS.get(a, "Attr%d_%d" % a)
                    i["perInstance"] = r.boolean()
                    if r.boolean(): i["precision"] = PRECISION[r.u8()]
                    if r.boolean(): i["interp"] = INTERP[r.u8()]
                    b["inputs"].append(i)
                b["sourceHash"] = r.u64()
                n = r.u32()
                b["offset"] = r.p
                b["size"] = n
                b["data"] = r.take(n)
                v["blobs"].append(b)
            ps["variants"].append(v)
        m["passes"][pn] = ps
    assert r.u64() == MAGIC, "bad trailer magic"
    assert r.p == len(data), "trailing bytes: %d" % (len(data) - r.p)
    return m


def dxbc_parts(code):
    """List the FourCC parts of a DXBC container (DXIL shaders carry a 'DXIL' part; SM5 ones carry SHDR/SHEX)."""
    if code[:4] != b"DXBC":
        return []
    n = struct.unpack_from("<I", code, 28)[0]
    offs = struct.unpack_from("<%dI" % n, code, 32)
    return [(code[o:o + 4].decode("latin1"), struct.unpack_from("<I", code, o + 4)[0]) for o in offs]


def bgfx_header(b):
    """Decode a bgfx shader blob (as stored in the .material.bin): 'VSH'|'FSH'|'CSH' + version(5), u32 inputHash,
    [u32 outputHash if VSH or version>=6], u16 nUniforms {u8 nameLen,name,u8 type,u8 num,u16 regIndex,u16 regCount
    [,u16 texInfo if version>=8]}, u32 codeSize, code, u8 0, u8 nAttr, nAttr*u16 attrId, u16 cbufSize.
    (v5: only ONE hash; regIndex = byte offset into the constant buffer, regCount = vec4 slots.)"""
    r = R(b)
    magic = r.take(3).decode("latin1")
    ver = r.u8()
    h = {"magic": magic, "version": ver, "inputHash": r.u32()}
    if ver >= 6:
        h["outputHash"] = r.u32()
    h["uniforms"] = []
    for _ in range(r.u16()):
        n = r.take(r.u8()).decode()
        t, num, ri, rc = r.u8(), r.u8(), r.u16(), r.u16()
        u = {"name": n, "type": "0x%02x" % t, "kind": UTYPES.get(t & 0xF, "?"), "fragment": bool(t & 0x10),
             "sampler": bool(t & 0x20), "num": num, "regIndex": ri, "regCount": rc}
        if ver >= 8:
            u["texInfo"] = r.u16()
        h["uniforms"].append(u)
    h["codeSize"] = r.u32()
    h["code"] = r.take(h["codeSize"])
    h["codeMagic"] = h["code"][:4]
    h["terminator"] = r.u8()
    h["dxbcParts"] = dxbc_parts(h["code"])
    h["attributes"] = [r.u16() for _ in range(r.u8())]  # written for every stage (FSH: 0)
    h["cbufSize"] = r.u16()
    h["trailing"] = len(b) - r.p
    return h


def bgfx_build(h):
    """Inverse of bgfx_header(): serialise a header dict (as returned by bgfx_header, with 'code' replaced as needed)."""
    o = bytearray(h["magic"].encode("latin1") + bytes([h["version"]]))
    o += struct.pack("<I", h["inputHash"])
    if h["version"] >= 6:
        o += struct.pack("<I", h["outputHash"])
    o += struct.pack("<H", len(h["uniforms"]))
    for u in h["uniforms"]:
        nm = u["name"].encode()
        o += bytes([len(nm)]) + nm + bytes([int(u["type"], 16), u["num"]]) + struct.pack("<HH", u["regIndex"], u["regCount"])
        if h["version"] >= 8:
            o += struct.pack("<H", u["texInfo"])
    o += struct.pack("<I", len(h["code"])) + h["code"] + b"\0"
    o += bytes([len(h["attributes"])]) + b"".join(struct.pack("<H", a) for a in h["attributes"])
    o += struct.pack("<H", h["cbufSize"])
    return bytes(o)


def cmd_info(m, full):
    print("%s  v%d  enc=%s" % (m["name"], m["version"], m["encryption"][::-1]))
    print("extra u16 (props->passes) = %d" % m["extra1"])
    print("samplers:")
    for n, s in m["samplers"].items():
        print("  %-24s reg=%d %s %s fmt=%r" % (n, s["reg"], s["type"], s["access"], s["format"]))
    print("props:")
    for n, f in m["props"].items():
        print("  %-34s %s x%s default=%s" % (n, f["type"], f.get("num", "-"), f.get("default")))
    for pn, ps in m["passes"].items():
        print("pass %s: bitset=%s fallback=%r blend=%s variants=%d extra2=%d" %
              (pn, ps["platformBitset"], ps["fallback"], ps["blend"], len(ps["variants"]), ps["extra2"]))
        print("   flags:", {k: v for k, v in ps["flagValues"].items()})
        plats = {}
        for v in ps["variants"]:
            for b in v["blobs"]:
                key = (b["platform"], b["stage"])
                a = plats.setdefault(key, [0, 0, 10 ** 9, 0])
                a[0] += 1; a[1] += b["size"]; a[2] = min(a[2], b["size"]); a[3] = max(a[3], b["size"])
        for (pl, st), a in sorted(plats.items()):
            print("   %-14s %-8s blobs=%-4d total=%-9d min=%-6d max=%d" % (pl, st, a[0], a[1], a[2], a[3]))
        if full:
            for vi, v in enumerate(ps["variants"]):
                print("   variant %d supported=%s %s" % (vi, v["supported"], v["flags"]))
                for b in v["blobs"]:
                    print("     %s/%s size=%d hash=%016x" % (b["platform"], b["stage"], b["size"], b["sourceHash"]))
                    for i in b["inputs"]:
                        print("        in %-28s %-5s %-10s inst=%s %s" % (i["name"], i["type"], i["semantic"],
                                                                         i["perInstance"], i.get("interp", "")))


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    cmd = argv[1]
    if cmd == "bgfx":
        h = bgfx_header(open(argv[2], "rb").read())
        for k, v in h.items():
            if k == "uniforms":
                for u in v: print("  uniform", u)
            else:
                print("%s: %s" % (k, "<%d bytes>" % len(v) if k == "code" else v))
        return 0
    m = parse(open(argv[2], "rb").read())
    if cmd == "selftest":  # every blob must re-serialise byte-exactly through bgfx_header/bgfx_build
        n = bad = 0
        for ps in m["passes"].values():
            for v in ps["variants"]:
                for b in v["blobs"]:
                    n += 1
                    if b["platform"] in ("Direct3D_SM40", "Direct3D_SM50", "Direct3D_SM60", "Direct3D_SM65"):
                        if bgfx_build(bgfx_header(b["data"])) != b["data"]:
                            bad += 1
        print("%s: %d blobs, %d bgfx round-trip mismatches" % (m["name"], n, bad))
    elif cmd == "info":
        cmd_info(m, False)
    elif cmd == "dump":
        if "--json" in argv:
            for ps in m["passes"].values():
                for v in ps["variants"]:
                    for b in v["blobs"]: b["data"] = len(b["data"])
            print(json.dumps(m, indent=1, default=str))
        else:
            cmd_info(m, True)
    elif cmd == "extract":
        import os
        out = argv[3]
        os.makedirs(out, exist_ok=True)
        opt = dict(zip(argv[4::2], argv[5::2]))
        want = dict(kv.split("=") for kv in opt.get("--flags", "").split(",") if kv)
        for pn, ps in m["passes"].items():
            if "--pass" in opt and opt["--pass"] != pn: continue
            for vi, v in enumerate(ps["variants"]):
                if any(v["flags"].get(k) != x for k, x in want.items()): continue
                for b in v["blobs"]:
                    if "--platform" in opt and opt["--platform"] != b["platform"]: continue
                    fn = "%s.%s.v%d.%s.%s.bin" % (m["name"], pn, vi, b["platform"], b["stage"])
                    open(os.path.join(out, fn), "wb").write(b["data"])
                    print(fn, b["size"], v["flags"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
