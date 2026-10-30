#!/usr/bin/env python3
"""interp_shaders: the frame interpolation's shaders (d9g_gpu_sdl.cpp, SA2_PLAYER_INTERP), GLSL
compiled to SPIR-V by the Android NDK's glslc and checked by its spirv-val, written as
d9g_interp_spirv.h.

The interpolation makes one frame between two the game drew, from the two pictures alone (no
depth, no motion vectors: the D3D9 stream has neither):

  luma    the frame at 1/scale of its size, as luma: the mean of a scale x scale box
          (scale/2 x scale/2 bilinear taps two texels apart), one 8-bit channel
  motion  for each 4 x 4 block of the new frame's luma (four 2 x 2 gathers), the displacement
          into the previous frame's within +-R luma texels whose sum of absolute differences
          is least: every second displacement, then the eight around the best of those (a
          small bias toward short vectors keeps a flat area still), and that least sum. The
          first version tried every displacement one texel at a time, 2,704 fetches a block,
          and held a Moto One 5G Ace's GPU to 27 frames a second at 1200x540
  warp    the frame half way: the content at uv was at uv + d/2 in the previous frame and is
          at uv - d/2 in the new one; the mean of the two where the block matched, the new
          frame's own pixel where it did not

SDL_gpu's SPIR-V resource sets: a fragment shader's samplers are set 2, its uniform buffers
set 3; its NDC has (-1, -1) at the lower left and its texture rows run down.

  .venv/bin/python host/d3d9gpu/interp_shaders.py           write the header
  .venv/bin/python host/d3d9gpu/interp_shaders.py --check   exit 1 when it is stale

glslc is private/android-sdk/ndk/<version>/shader-tools/linux-x86_64/glslc (SA2_GLSLC names
another); its output is a function of the source and its version, both named in the header.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
HEADER = HERE / "d9g_interp_spirv.h"
RADIUS = 6        # the motion search's reach in luma texels, each way
BLOCK = 4         # a motion block's side in luma texels

VERTEX = """#version 450
// One triangle over the whole target; uv (0, 0) at its top left.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

LUMA = """#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_luma;
layout(set = 2, binding = 0) uniform sampler2D src;     // the frame, bilinear
layout(set = 3, binding = 0) uniform Params {
    vec4 src_texel;     // xy: one source texel in uv
    vec4 scale;         // x: the box's side in source texels (2, 4 or 8)
};
void main() {
    int taps = max(1, int(scale.x) / 2);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 4; j++) {
        if (j >= taps) break;
        for (int i = 0; i < 4; i++) {
            if (i >= taps) break;
            vec2 off = (vec2(float(i), float(j)) - 0.5 * float(taps - 1)) * 2.0;
            sum += texture(src, v_uv + off * src_texel.xy).rgb;
        }
    }
    float y = dot(sum / float(taps * taps), vec3(0.299, 0.587, 0.114));
    o_luma = vec4(y, 0.0, 0.0, 1.0);
}
"""

MOTION = f"""#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_motion;
layout(set = 2, binding = 0) uniform sampler2D cur;     // the new frame's luma (clamped at its edges)
layout(set = 2, binding = 1) uniform sampler2D prev;    // the previous frame's luma
layout(set = 3, binding = 0) uniform Params {{
    vec4 size;          // xy: the luma's size in texels; zw: the motion target's
}};
const int R = {RADIUS};
// A 4 x 4 block of texels from `base` as four gathers of 2 x 2: texels (i, j) to (i + 1, j + 1)
// share the corner whose uv is (i + 1, j + 1) / size.
vec4 g0(sampler2D t, vec2 base, vec2 inv) {{ return textureGather(t, (base + vec2(1.0, 1.0)) * inv); }}
vec4 g1(sampler2D t, vec2 base, vec2 inv) {{ return textureGather(t, (base + vec2(3.0, 1.0)) * inv); }}
vec4 g2(sampler2D t, vec2 base, vec2 inv) {{ return textureGather(t, (base + vec2(1.0, 3.0)) * inv); }}
vec4 g3(sampler2D t, vec2 base, vec2 inv) {{ return textureGather(t, (base + vec2(3.0, 3.0)) * inv); }}
void main() {{
    vec2 inv = 1.0 / size.xy;
    vec2 base = floor(v_uv * size.zw) * 4.0;
    vec4 c0 = g0(cur, base, inv), c1 = g1(cur, base, inv), c2 = g2(cur, base, inv), c3 = g3(cur, base, inv);
    // The sum of absolute differences at displacement d, and a small bias toward short vectors
    // so a flat area keeps still: the still block first, then every second displacement
    // within R, then the eight around the best of those.
    vec4 e = abs(c0 - g0(prev, base, inv)) + abs(c1 - g1(prev, base, inv)) +
             abs(c2 - g2(prev, base, inv)) + abs(c3 - g3(prev, base, inv));
    float best = e.x + e.y + e.z + e.w;
    vec2 at = vec2(0.0);
    for (int dy = -R; dy <= R; dy += 2) {{
        for (int dx = -R; dx <= R; dx += 2) {{
            vec2 d = vec2(float(dx), float(dy));
            vec2 b = base + d;
            e = abs(c0 - g0(prev, b, inv)) + abs(c1 - g1(prev, b, inv)) +
                abs(c2 - g2(prev, b, inv)) + abs(c3 - g3(prev, b, inv));
            float s = e.x + e.y + e.z + e.w + 0.02 * length(d);
            if (s < best) {{ best = s; at = d; }}
        }}
    }}
    vec2 coarse = at;
    for (int dy = -1; dy <= 1; dy++) {{
        for (int dx = -1; dx <= 1; dx++) {{
            if (dx == 0 && dy == 0) continue;
            vec2 d = coarse + vec2(float(dx), float(dy));
            vec2 b = base + d;
            e = abs(c0 - g0(prev, b, inv)) + abs(c1 - g1(prev, b, inv)) +
                abs(c2 - g2(prev, b, inv)) + abs(c3 - g3(prev, b, inv));
            float s = e.x + e.y + e.z + e.w + 0.02 * length(d);
            if (s < best) {{ best = s; at = d; }}
        }}
    }}
    o_motion = vec4(at, best / 16.0, 1.0);
}}
"""

WARP = """#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D prev;    // the previous frame, bilinear
layout(set = 2, binding = 1) uniform sampler2D cur;     // the new frame, bilinear
layout(set = 2, binding = 2) uniform sampler2D motion;  // per block: luma texels to the previous frame, and the mean difference
layout(set = 3, binding = 0) uniform Params {
    vec4 k;             // xy: one luma texel in uv; z: the mean difference past which a block did not match
};
void main() {
    vec4 m = texture(motion, v_uv);
    vec2 d = m.xy * k.xy;
    vec4 a = texture(prev, v_uv + 0.5 * d);
    vec4 b = texture(cur, v_uv - 0.5 * d);
    float matched = clamp(1.0 - m.z / k.z, 0.0, 1.0);
    o_color = mix(texture(cur, v_uv), 0.5 * (a + b), matched);
}
"""


def tool(name: str) -> Path:
    env = os.environ.get("SA2_" + name.upper().replace("-", "_"))
    if env:
        return Path(env)
    found = sorted((ROOT / "private/android-sdk/ndk").glob(f"*/shader-tools/linux-x86_64/{name}"))
    if not found:
        sys.exit(f"no {name} under private/android-sdk/ndk/*/shader-tools (set SA2_{name.upper()})")
    return found[-1]


def compile_stage(glslc: Path, val: Path, name: str, stage: str, source: str, tmp: Path) -> bytes:
    src = tmp / f"{name}.{stage}"
    out = tmp / f"{name}.{stage}.spv"
    src.write_text(source)
    subprocess.run([str(glslc), "--target-env=vulkan1.0", "-O", "-fshader-stage=" + stage, str(src), "-o", str(out)],
                   check=True)
    subprocess.run([str(val), "--target-env", "vulkan1.0", str(out)], check=True)
    return out.read_bytes()


def words(blob: bytes) -> str:
    assert len(blob) % 4 == 0
    vals = [int.from_bytes(blob[i:i + 4], "little") for i in range(0, len(blob), 4)]
    lines = []
    for i in range(0, len(vals), 8):
        lines.append("    " + ", ".join(f"0x{v:08x}u" for v in vals[i:i + 8]) + ",")
    return "\n".join(lines)


def render() -> str:
    glslc, val = tool("glslc"), tool("spirv-val")
    version = subprocess.run([str(glslc), "--version"], check=True, capture_output=True, text=True).stdout
    version = version.splitlines()[0].strip()
    with tempfile.TemporaryDirectory() as t:
        blobs = {"kVertex": compile_stage(glslc, val, "fullscreen", "vert", VERTEX, Path(t)),
                 "kLuma": compile_stage(glslc, val, "luma", "frag", LUMA, Path(t)),
                 "kMotion": compile_stage(glslc, val, "motion", "frag", MOTION, Path(t)),
                 "kWarp": compile_stage(glslc, val, "warp", "frag", WARP, Path(t))}
    body = "".join(f"alignas(4) constexpr uint32_t {k}[] = {{\n{words(v)}\n}};\n\n" for k, v in blobs.items())
    return (f"// d9g_interp_spirv.h: generated by host/d3d9gpu/interp_shaders.py ({version}); do not edit.\n"
            f"// The GLSL is in that script; --check holds this file to it.\n"
            f"#pragma once\n#include <cstdint>\n\nnamespace sa2_interp_spirv {{\n\n"
            f"constexpr int kRadius = {RADIUS};\nconstexpr int kBlock = {BLOCK};\n\n"
            f"{body}}}  // namespace sa2_interp_spirv\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 when the header is not this script's output")
    args = ap.parse_args()
    text = render()
    if args.check:
        same = HEADER.is_file() and HEADER.read_text() == text
        print(f"{HEADER.name}: {'current' if same else 'STALE'}")
        return 0 if same else 1
    HEADER.write_text(text)
    print(f"wrote {HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
