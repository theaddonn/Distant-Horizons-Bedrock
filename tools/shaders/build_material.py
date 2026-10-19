#!/usr/bin/env python3
"""build_material.py - swap shader code of one pass in an UNPACKED vanilla .material.bin and repack it (Stage B).

Works on the JSON+blob directory produced by `mbt.jar -u` (see docs/spikes/shader-toolchain.md): <dir>/<Name>.json,
<dir>/<Pass>/<Pass>.json and <dir>/<Pass>/<n>.<Platform>.<Stage>.dxbc.  The bgfx v5 blob header (VSH/FSH, hash, uniform
table, attribute ids, cbuf size) is held as structured fields in <Pass>.json, the .dxbc file is only the native code, so
this tool edits those fields; the result is then serialised by MaterialBinTool into the v5 blob (identical to
matbin_inspect.bgfx_build, which `--verify` uses to check the finished file).

Modes
  --tint-only      B2: only replace code (and optionally uniform tables) of the selected variants; keep every
                   other pass/variant, the material name and all inputs.
  --new-material   B3: requires --rename NAME; keeps only --keep-passes (default: the --pass pass), renames the
                   material (json "name" + output file), optionally adds property fields (--add-prop).

Common options
  --unpacked DIR       unpacked material dir (contains <Name>.json), e.g. ~/.cache/dh-tools/unpacked/RenderChunk
  --pass NAME          pass whose shaders are replaced (all variants, or only those matching --flags)
  --flags K=V,K=V      only variants whose flag values match (default: all variants)
  --code PLAT:STAGE=FILE   native code for every selected variant, e.g. Direct3D_SM65:Fragment=ps.dxil (repeatable)
  --drop-platforms     remove shader blobs of platforms for which no --code was given (else vanilla code is kept)
  --uniforms STAGE=SPEC    replace the bgfx uniform table of that stage (Vertex|Fragment) in the selected variants.
                       SPEC = name:type:num:regIndex:regCount[,...] ; type = vec4|vec4f|mat3|mat4|mat4f|sampler|<int>
                       (f = fragment-only flag 0x10; sampler = 0x30; vec4=2, mat3=3, mat4=4). regIndex = byte offset.
  --cbuf STAGE=BYTES   replace the constant-buffer size field of that stage
  --attrs STAGE=a,b,c  replace the bgfx attribute id list of that stage
  --add-prop N=x,y,z,w add a vec4 property field with that default (new-material; repeatable)
  --rename NAME        new material name (json name + <NAME>.material.bin)
  --out DIR            output directory (receives <Name>.material.bin)
  --mbt JAR --java BIN MaterialBinTool jar (default ~/.cache/dh-tools/mbt-build/mbt.jar) and java binary
  --verify             parse the result with matbin_inspect and re-serialise every blob
"""
import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import matbin_inspect as mi  # noqa: E402

HOME = os.path.expanduser("~")
TYPES = {"sampler": 0x30, "vec4": 2, "vec4f": 0x12, "mat3": 3, "mat3f": 0x13, "mat4": 4, "mat4f": 0x14}


def parse_uniforms(spec):
    out = []
    for item in spec.split(","):
        n, t, num, ri, rc = item.split(":")
        out.append({"name": n, "type": TYPES[t] if t in TYPES else int(t, 0), "num": int(num), "regIndex": int(ri),
                    "regCount": int(rc)})
    return out


def split_kv(s, what):
    if "=" not in s:
        sys.exit("bad %s %r (want KEY=VALUE)" % (what, s))
    return s.split("=", 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tint-only", action="store_true")
    mode.add_argument("--new-material", action="store_true")
    ap.add_argument("--unpacked", required=True)
    ap.add_argument("--pass", dest="pass_name", required=True)
    ap.add_argument("--flags", default="")
    ap.add_argument("--code", action="append", default=[])
    ap.add_argument("--drop-platforms", action="store_true")
    ap.add_argument("--uniforms", action="append", default=[])
    ap.add_argument("--cbuf", action="append", default=[])
    ap.add_argument("--attrs", action="append", default=[])
    ap.add_argument("--add-prop", action="append", default=[])
    ap.add_argument("--keep-passes", default="")
    ap.add_argument("--rename", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--mbt", default=os.path.join(HOME, ".cache/dh-tools/mbt-build/mbt.jar"))
    ap.add_argument("--java", default="/usr/lib/jvm/java-21-openjdk-amd64/bin/java")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    if a.new_material and not a.rename:
        sys.exit("--new-material requires --rename")
    if a.tint_only and (a.rename or a.keep_passes or a.add_prop):
        sys.exit("--tint-only keeps the material as is: no --rename/--keep-passes/--add-prop")
    code = {}
    for c in a.code:
        k, f = split_kv(c, "--code")
        plat, stage = k.split(":")
        code[(plat, stage)] = open(f, "rb").read()
    uni = {k: parse_uniforms(v) for k, v in (split_kv(x, "--uniforms") for x in a.uniforms)}
    cbuf = {k: int(v) for k, v in (split_kv(x, "--cbuf") for x in a.cbuf)}
    attrs = {k: [int(i) for i in v.split(",") if i] for k, v in (split_kv(x, "--attrs") for x in a.attrs)}
    want = dict(kv.split("=") for kv in a.flags.split(",") if kv)

    src = os.path.abspath(a.unpacked)
    name = os.path.basename(src)
    root = json.load(open(os.path.join(src, name + ".json")))
    edit_passes = [x for x in a.pass_name.split(",") if x]
    for x in edit_passes:
        if x not in root["passes"]:
            sys.exit("pass %r not in %s" % (x, root["passes"]))
    new_name = a.rename or root["name"]

    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="bm_", dir=os.path.abspath(a.out))
    try:
        dst = os.path.join(work, new_name)
        keep = [p for p in (a.keep_passes.split(",") if a.keep_passes else edit_passes) if p] \
            if a.new_material else list(root["passes"])
        for x in edit_passes:
            if x not in keep:
                keep.append(x)
        os.makedirs(dst)
        for p in keep:
            shutil.copytree(os.path.join(src, p), os.path.join(dst, p))
        root["passes"] = [p for p in root["passes"] if p in keep]
        root["name"] = new_name
        for pn, spec in (split_kv(x, "--add-prop") for x in a.add_prop):
            v = [float(f) for f in spec.split(",")]
            assert len(v) == 4, "--add-prop wants 4 floats"
            root["propertyFieldMap"][pn] = {"num": 1, "type": 2, "hasData": True, "vectorData": v}
        json.dump(root, open(os.path.join(dst, new_name + ".json"), "w"), indent=2)

        for pass_name in edit_passes:
            pj_path = os.path.join(dst, pass_name, pass_name + ".json")
            pj = json.load(open(pj_path))
            nvar = nblob = ndrop = 0
            for vi, v in enumerate(pj["variantList"]):
                if any(v["flags"].get(k) != x for k, x in want.items()):
                    continue
                nvar += 1
                keep_codes = []
                for sc in v["shaderCodes"]:
                    st = sc["platformShaderStage"]
                    key = (st["platform"], st["stage"])
                    d = sc["shaderCode"]["bgfxShaderData"]
                    if key in code:
                        fn = d["codeFile"]
                        open(os.path.join(dst, pass_name, fn), "wb").write(code[key])
                        nblob += 1
                    elif a.drop_platforms and not any(p == key[0] for p, _ in code):
                        ndrop += 1
                        os.remove(os.path.join(dst, pass_name, d["codeFile"]))
                        continue
                    if key[1] in uni:
                        d["uniforms"] = copy.deepcopy(uni[key[1]])
                    if key[1] in cbuf:
                        d["size"] = cbuf[key[1]]
                    if key[1] in attrs:
                        d["attrs"] = list(attrs[key[1]])
                    keep_codes.append(sc)
                v["shaderCodes"] = keep_codes
            if not nvar:
                sys.exit("no variant matches --flags %r" % a.flags)
            json.dump(pj, open(pj_path, "w"), indent=2)
            print("pass %s: %d variants edited, %d blobs replaced, %d blobs dropped" % (pass_name, nvar, nblob, ndrop))

        outtmp = os.path.join(work, "_out")
        os.makedirs(outtmp)
        subprocess.run([a.java, "-jar", a.mbt, "-r", dst, "-o", outtmp], check=True)
        res = os.path.join(outtmp, new_name + ".material.bin")
        if not os.path.isfile(res):
            sys.exit("mbt produced no %s (got %s)" % (res, os.listdir(outtmp)))
        final = os.path.join(a.out, new_name + ".material.bin")
        shutil.copyfile(res, final)
        print("wrote %s (%d bytes)" % (final, os.path.getsize(final)))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if a.verify:
        m = mi.parse(open(final, "rb").read())
        n = 0
        for ps in m["passes"].values():
            for v in ps["variants"]:
                for b in v["blobs"]:
                    if b["platform"].startswith("Direct3D_SM"):
                        h = mi.bgfx_header(b["data"])
                        assert mi.bgfx_build(h) == b["data"], "bgfx round-trip mismatch"
                        n += 1
        print("verify ok: %s name=%s passes=%s, %d bgfx blobs re-serialise exactly" % (final, m["name"], list(m["passes"]), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
