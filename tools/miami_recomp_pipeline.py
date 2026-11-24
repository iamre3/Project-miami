#!/usr/bin/env python3
"""Project Miami — Real Setup Recompilation Driver using E:\\ProjectMiami\\tools.

Executes Leonida's RDNA2 -> SPIR-V shader translation (rdna2spirv.py),
SPIR-V basic-block peephole optimization (spirv_opt.py), and pure-Python
SPIR-V binary assembly (spirv_asm.py), writing compiled .spv modules and
recompilation_manifest.json into the game's recompiled cache directory.
"""
import json
import os
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)

import rdna2spirv  # noqa: E402
import spirv_asm   # noqa: E402
import spirv_opt   # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: miami_recomp_pipeline.py <disasm_dir> <out_dir>", file=sys.stderr)
        return 2

    disasm_dir = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    rdna2spirv.WAVE64_EMULATE = True
    rdna2spirv.OPT_SPIRV = True

    entries = sorted(f for f in os.listdir(disasm_dir) if f.endswith(".s"))
    manifest = {
        "engine": "Leonida RDNA2-to-SPIR-V Recompilation Pipeline (E:\\ProjectMiami\\tools)",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "translated_count": 0,
        "total_spirv_bytes": 0,
        "modules": {},
    }

    for fn in entries:
        path = os.path.join(disasm_dir, fn)
        stem = os.path.splitext(fn)[0]
        asm_text = rdna2spirv.translate(path)
        asm_text = spirv_opt.optimize(asm_text)

        spvasm_path = os.path.join(out_dir, stem + ".spvasm")
        spv_path = os.path.join(out_dir, stem + ".spv")
        with open(spvasm_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(asm_text)

        spv_bytes = spirv_asm.assemble(asm_text, target_env="vulkan1.2")
        with open(spv_path, "wb") as f:
            f.write(spv_bytes)

        magic = int.from_bytes(spv_bytes[:4], "little")
        manifest["translated_count"] += 1
        manifest["total_spirv_bytes"] += len(spv_bytes)
        manifest["modules"][stem] = {
            "spv_file": stem + ".spv",
            "spvasm_file": stem + ".spvasm",
            "spv_size_bytes": len(spv_bytes),
            "spirv_magic_hex": f"0x{magic:08X}",
        }
        print(f"[RECOMP] {stem}.s -> {stem}.spv ({len(spv_bytes)} bytes, magic=0x{magic:08X})")

    manifest_path = os.path.join(out_dir, "recompilation_manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"[RECOMP] Completed: {manifest['translated_count']} RDNA2 shaders compiled to SPIR-V ({manifest['total_spirv_bytes']} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
