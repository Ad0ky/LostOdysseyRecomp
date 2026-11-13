#!/usr/bin/env python3
"""Precompile the runtime's builtin host shaders to SPIR-V for the Switch.

The PC runtime compiles its own host shaders (presentation, blits, SMAA, TAA,
AO, ...) from HLSL with DXC at first use and caches the result. DXC does not
run on Horizon, so the Switch build embeds those binaries instead
(LostOdysseyRecomp/os/switch/builtin_spirv.inc, looked up by the same FNV-1a
key as CompileCachedHlsl).

How it works:
  1. The HLSL sources are taken from the runtime sources themselves: string
     expressions are copied verbatim into a small host C++ program, which
     prints each source exactly as the runtime builds it.
  2. Each (source, entry, profile) is compiled with the pinned DXC
     (tools/XenosRecomp/thirdparty/dxc-bin) using CompileCachedHlsl's
     Vulkan arguments.
  3. The SPIR-V words are written to the .inc file.

Run from the repository root (Linux x64, or macOS with the pinned DXC's macOS
build), naming the sources that changed:
    python3 tools/switch/gen-builtin-spirv.py --refresh presentation
--refresh takes the names harvest_program() gives the sources (presentation,
presentationUi, transferPs, transferPair, blitPs, ...; "all" for every one). Only
shaders made from those sources are replaced or added; every other entry of the
table stays as it is. The table also holds shaders this script may not harvest
the way the runtime builds them (SMAA): a run that rewrote the whole table from
a macOS harvest lost them, and the game then failed to start on the console
("SMAA edge: dxcompiler library not available", 2026-11-13). --add-only never
drops or replaces an entry: it only adds the refreshed shaders the table lacks.
The EDRAM transfer shaders come from edram_transfer.h's TransferSource() and
TransferPairSource(), the functions the renderer compiles them from.
A shader the Switch build cannot find is written to state/logs/missing-shaders
on the SD card: copy the file unchanged into tools/switch/extra-builtin-shaders/
and re-run.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "LostOdysseyRecomp" / "gpu"
DXC_DIR = ROOT / "tools" / "XenosRecomp" / "thirdparty" / "dxc-bin"
OUTPUT = Path(os.environ.get("LO_BUILTIN_SPIRV_OUT") or ROOT / "LostOdysseyRecomp" / "os" / "switch" / "builtin_spirv.inc")
# Sources the console dumped to state/logs/missing-shaders (built at run time,
# e.g. from a game shader), kept as <key>_<entry>_<profile>.hlsl.
EXTRA = ROOT / "tools" / "switch" / "extra-builtin-shaders"
KEY_SUFFIX = b"lo-dxc-vulkan12-dx-layout-v1"


def extract_expression(path, anchor, start_pattern):
    """Text of the initializer that follows start_pattern (after anchor)."""
    text = path.read_text(encoding="utf-8")
    base = 0
    if anchor:
        base = text.index(anchor)
    match = re.compile(start_pattern).search(text, base)
    if not match:
        raise SystemExit(f"{path.name}: '{start_pattern}' not found after '{anchor}'")
    i = match.end()
    out_start = i
    depth = 0
    while i < len(text):
        c = text[i]
        if c == "R" and text[i + 1] == '"':
            close = text.index("(", i)
            delim = text[i + 2:close]
            end = text.index(")" + delim + '"', close)
            i = end + len(delim) + 2
            continue
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth == 0:
            return text[out_start:i].strip()
        i += 1
    raise SystemExit(f"{path.name}: unterminated initializer for '{start_pattern}'")


def harvest_program():
    """C++ source that prints every builtin shader as entry\\0profile\\0size\\0source."""
    rel = lambda p: os.path.relpath(p, GPU).replace(os.sep, "/")
    locals_ = {
        "presentation": extract_expression(GPU / "presentation.cpp", None, r"const char \*source\s*=\s*"),
        "presentationUi": extract_expression(GPU / "presentation.cpp", None, r"const char \*uiSource\s*=\s*"),
        "presentationOverlay": extract_expression(GPU / "presentation.cpp", None, r"const char \*overlaySource\s*=\s*"),
        # The EDRAM transfer shaders: CompileTransferShader takes their sources from edram_transfer.h's functions.
        "transferPs": "gpu::edram_transfer::TransferSource()",
        "blitVs": extract_expression(GPU / "renderer.cpp", "void CompileBlitShaders()", r"const char\* vsSrc\s*=\s*"),
        "blitPs": extract_expression(GPU / "renderer.cpp", "void CompileBlitShaders()", r"const char\* psSrc\s*=\s*"),
        "rectListGs": extract_expression(GPU / "renderer.cpp", "void CompileRectListGs()", r"std::string src\s*=\s*"),
        "temporalAa": extract_expression(GPU / "temporal_aa.cpp", None, r"\nconst char\* source\s*=\s*"),
        "motionMask": extract_expression(GPU / "motion_replay_gpu.h", None, r"static constexpr const char\* kMaskShader\s*=\s*"),
        "smaaHead": extract_expression(GPU / "shader" / "smaa_pipeline.h", None, r"std::string hlsl\s*=\s*"),
        "smaaTail": extract_expression(GPU / "shader" / "smaa_pipeline.h", None, r"hlsl\+=(?=R)"),
    }
    lines = [
        "#include <cstdio>",
        "#include <string>",
        f'#include "{rel(GPU / "bloom_prefilter.h")}"',
        f'#include "{rel(GPU / "edram_transfer.h")}"',
        f'#include "{rel(GPU / "fsr1_shaders.h")}"',
        f'#include "{rel(GPU / "scene_copy_promotion_shaders.h")}"',
        f'#include "{rel(GPU / "shader" / "ambient_occlusion_hlsl.h")}"',
        '#include "../../thirdparty/smaa/SMAA_source.h"',
    ]
    for name, expr in locals_.items():
        lines.append(f"static const std::string {name} = {expr};")
    lines += [
        "static const char* group = \"\";",
        "static void Emit(const std::string& s, const char* entry, const char* profile) {",
        "    std::printf(\"%s%c%s%c%s%c%zu%c\", group, 0, entry, 0, profile, 0, s.size(), 0);",
        "    std::fwrite(s.data(), 1, s.size(), stdout);",
        "}",
        "static std::string Smaa() {",
        "    // shader/smaa_pipeline.h, Vulkan branch",
        "    std::string hlsl = smaaHead, smaa = smaaSource;",
        '    smaa.insert(smaa.find("SamplerState LinearSampler"), "[[vk::binding(3,0)]] ");',
        '    smaa.insert(smaa.find("SamplerState PointSampler"), "[[vk::binding(4,0)]] ");',
        "    return hlsl + smaa + smaaTail;",
        "}",
        "int main() {",
        '    Emit(presentation, "vertex", "vs_6_0");',
        '    Emit(presentation, "pixel", "ps_6_0");',
        '    Emit(presentation, "pixelLinear", "ps_6_0");',
        '    Emit(presentation, "pixelCubic", "ps_6_0");',
        '    Emit(presentation, "gainPixel", "ps_6_0");',
        '    // Presentation::Impl::FsrPipeline: the presentation source with FSR 1 appended',
        '    Emit(presentation + gpu::fsr1::Shader, "easuPixel", "ps_6_0");',
        '    Emit(presentation + gpu::fsr1::Shader, "rcasPixel", "ps_6_0");',
        '    Emit(presentationUi, "vertexUi", "vs_6_0");',
        '    Emit(presentationUi, "pixelUi", "ps_6_0");',
        '    Emit(presentationOverlay, "vertexOverlay", "vs_6_0");',
        '    Emit(presentationOverlay, "pixelOverlay", "ps_6_0");',
        '    Emit(transferPs, "main", "ps_6_0");',
        "    // One transfer shader per pair of unequal classes (renderer.cpp, CompileTransferShader).",
        '    for (unsigned s = 0; s <= gpu::edram_transfer::kLastClass; ++s)',
        '        for (unsigned d = 0; d <= gpu::edram_transfer::kLastClass; ++d)',
        '            if (s != d) { group = "transferPair"; Emit(gpu::edram_transfer::TransferPairSource(s, d), "main", "ps_6_0"); }',
        '    Emit(blitVs, "main", "vs_6_0");',
        '    Emit(blitPs, "main", "ps_6_0");',
        '    Emit(gpu::bloom_prefilter::PixelShader, "main", "ps_6_0");',
        '    Emit(gpu::scene_copy_promotion::RgbaShader, "main", "ps_6_0");',
        '    Emit(gpu::scene_copy_promotion::RgbShader, "main", "ps_6_0");',
        '    Emit(rectListGs, "main", "gs_6_0");',
        '    Emit(temporalAa, "vertex", "vs_6_0");',
        '    Emit(temporalAa, "pixel", "ps_6_0");',
        '    Emit(temporalAa, "displayPixel", "ps_6_0");',
        '    Emit(gpu::ao::Shader, "vertex", "vs_6_0");',
        '    Emit(gpu::ao::Shader, "visibility", "ps_6_0");',
        '    Emit(gpu::ao::Shader, "composite", "ps_6_0");',
        '    Emit(gpu::ao::Shader, "compositePair", "ps_6_0");',
        '    Emit(motionMask, "vertex", "vs_6_0");',
        '    Emit(motionMask, "pixel", "ps_6_0");',
        '    for (const char* e : {"edge", "weight", "neighborhood"}) Emit(Smaa(), e, "ps_6_0");',
        "}",
    ]
    # Each Emit is tagged with the name of its source: the first of the sources above its expression names, else
    # the expression's first identifier (gpu::bloom_prefilter::PixelShader, Smaa).
    tagged = []
    for line in lines:
        emit = line.find("Emit(")
        if emit >= 0 and not line.startswith("static void Emit") and "group =" not in line:
            expression = line[emit + 5:]
            names = [n for n in locals_ if re.search(rf"\b{n}\b", expression)]
            name = min(names, key=lambda n: re.search(rf"\b{n}\b", expression).start()) if names else \
                re.match(r"[\w:]+", expression).group(0)
            line = line[:emit] + f'group = "{name}", ' + line[emit:]
        tagged.append(line)
    return "\n".join(tagged) + "\n"


def load_table(path):
    """The table as it is: [(key, entry, profile, bytes)] in its order."""
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8")
    arrays = {m.group(1): m.group(2) for m in
              re.finditer(r"static const uint32_t (kBuiltinSpirv\d+)\[\] = \{(.*?)\};", text, re.S)}
    entries = []
    for m in re.finditer(r'\{0x([0-9a-f]+)ull, "(\w+)", "(\w+)", (kBuiltinSpirv\d+), sizeof', text):
        words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]+)", arrays[m.group(4)])]
        entries.append((int(m.group(1), 16), m.group(2), m.group(3), struct.pack(f"<{len(words)}I", *words)))
    return entries


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--refresh", default="",
                        help="comma-separated sources whose shaders to compile and replace, or 'all'")
    parser.add_argument("--list", action="store_true", help="print the sources' names and stop")
    parser.add_argument("--add-only", action="store_true",
                        help="never drop or replace an entry of the table: only add the refreshed shaders it lacks")
    options = parser.parse_args()
    if not options.refresh and not options.list:
        parser.error("--refresh <sources> (or --list)")
    refresh = set(options.refresh.split(",")) - {""}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = GPU / "_switch_builtin_harvest.cpp"  # next to the headers it includes
        src.write_text(harvest_program(), encoding="utf-8")
        try:
            exe = tmp / "harvest"
            subprocess.run(["g++", "-std=c++20", "-O0", "-o", str(exe), str(src)], check=True)
        finally:
            src.unlink()
        blob = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE).stdout

        shaders, pos = [], 0
        while pos < len(blob):
            group_end = blob.index(b"\0", pos)
            entry_end = blob.index(b"\0", group_end + 1)
            profile_end = blob.index(b"\0", entry_end + 1)
            size_end = blob.index(b"\0", profile_end + 1)
            group = blob[pos:group_end].decode()
            entry = blob[group_end + 1:entry_end].decode()
            profile = blob[entry_end + 1:profile_end].decode()
            size = int(blob[profile_end + 1:size_end])
            source = blob[size_end + 1:size_end + 1 + size]
            pos = size_end + 1 + size
            shaders.append((source, entry, profile, group))
        if options.list:
            print("sources:", ", ".join(sorted({s[3] for s in shaders} | {"extra"})))
            return
        unknown = refresh - {s[3] for s in shaders} - {"all", "extra"}
        if unknown:
            raise SystemExit(f"--refresh: no source named {', '.join(sorted(unknown))} (--list shows them)")

        for path in sorted(EXTRA.glob("*.hlsl")):
            key_hex, rest = path.stem.split("_", 1)
            parts = rest.rsplit("_", 3)  # profile is "<stage>_<major>_<minor>"
            entry, profile = parts[0], "_".join(parts[1:])
            source = path.read_bytes()
            key = fnv1a64(source + b"\0" + entry.encode() + b"\0" + profile.encode() + KEY_SUFFIX)
            if f"{key:016x}" != key_hex:
                raise SystemExit(f"{path.name}: content hashes to {key:016x} (file changed after the dump?)")
            shaders.append((source, entry, profile, "extra"))

        # The pinned DXC: Linux x64, or macOS (the same compiler build; its SPIR-V was the same bytes for every
        # shader of this table on 2026-11-13).
        if sys.platform == "darwin":
            arch = "arm64" if os.uname().machine == "arm64" else "x64"
            dxc = DXC_DIR / "bin" / arch / "dxc-macos"
            env = dict(os.environ, DYLD_LIBRARY_PATH=str(DXC_DIR / "lib" / arch))
        else:
            dxc = DXC_DIR / "bin" / "x64" / "dxc-linux"
            env = dict(os.environ, LD_LIBRARY_PATH=str(DXC_DIR / "lib" / "x64"))
        # Every harvested shader's key (what the sources make now), and the compiled ones of the refreshed sources.
        harvested = {fnv1a64(source + b"\0" + entry.encode() + b"\0" + profile.encode() + KEY_SUFFIX)
                     for source, entry, profile, group in shaders}
        records = []
        for index, (source, entry, profile, group) in enumerate(shaders):
            if "all" not in refresh and group not in refresh:
                continue
            hlsl = tmp / f"{index}.hlsl"
            spv = tmp / f"{index}.spv"
            hlsl.write_bytes(source)
            args = [str(dxc), "-E", entry, "-T", profile, "-HV", "2021",
                    "-Wno-parentheses-equality", "-Wno-unused-value", "-all-resources-bound",
                    "-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"]
            if profile.startswith("vs_"):
                args.append("-fvk-invert-y")
            args += ["-O3", "-Qstrip_debug", "-Fo", str(spv), str(hlsl)]
            result = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if result.returncode != 0:
                raise SystemExit(f"{group} {entry} {profile}: {result.stderr.strip()[:1200]}")
            key = fnv1a64(source + b"\0" + entry.encode() + b"\0" + profile.encode() + KEY_SUFFIX)
            records.append((key, entry, profile, spv.read_bytes()))
            print(f"{key:016x} {profile} {entry}: {spv.stat().st_size} bytes")

    # The table as it was, without the entries the refreshed sources no longer make: an entry goes only when no
    # harvested shader has its key and a refreshed shader has its entry point's name and profile. Then the new ones.
    names = {(entry, profile) for _, entry, profile, _ in records}
    kept, dropped = [], []
    for old in load_table(OUTPUT):
        (dropped if not options.add_only and old[0] not in harvested and (old[1], old[2]) in names else kept).append(old)
    old_bytes = {key: data for key, _, _, data in kept}
    for key, entry, profile, data in records:
        if key in old_bytes and old_bytes[key] != data:
            print(f"CHANGED {key:016x} {profile} {entry}: other bytes from the same source (another DXC?)")
    by_key = {r[0]: r for r in records}
    added = [r for r in records if r[0] not in old_bytes]
    records = [old if options.add_only else by_key.get(old[0], old) for old in kept] + added
    for key, entry, profile, _ in dropped:
        print(f"dropped {key:016x} {profile} {entry} (its source changed)")
    for key, entry, profile, _ in added:
        print(f"added   {key:016x} {profile} {entry}")
    seen, out = set(), [
        "// Generated by tools/switch/gen-builtin-spirv.py; do not edit.",
        "// Builtin host shaders as SPIR-V for the Switch (no DXC on the console).",
        "// Key: FNV-1a 64 of source '\\0' entry '\\0' profile \"lo-dxc-vulkan12-dx-layout-v1\".",
    ]
    table = []
    for i, (key, entry, profile, data) in enumerate(records):
        if key in seen:
            continue
        seen.add(key)
        words = struct.unpack(f"<{len(data) // 4}I", data)
        out.append(f"static const uint32_t kBuiltinSpirv{i}[] = {{")
        for j in range(0, len(words), 8):
            out.append("    " + ",".join(f"0x{w:08x}" for w in words[j:j + 8]) + ",")
        out.append("};")
        table.append(f'    {{0x{key:016x}ull, "{entry}", "{profile}", kBuiltinSpirv{i}, sizeof(kBuiltinSpirv{i})}},')
    out.append("static const BuiltinSpirv kBuiltinSpirv[] = {")
    out += table
    out.append("};")
    OUTPUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {OUTPUT.relative_to(ROOT) if OUTPUT.is_relative_to(ROOT) else OUTPUT}: {len(table)} shaders")


if __name__ == "__main__":
    main()
