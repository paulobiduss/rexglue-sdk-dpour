#!/usr/bin/env bash
# Compile the Downpour-added guest-output paint shaders from HLSL to SPIR-V and
# emit them as C headers in the same format as the upstream ones.
#
# The upstream Xenia shaders in src/ui/shaders/vulkan_spirv/ ship pre-built and
# are not regenerated here - only the three Downpour effects that were
# originally written for D3D12 alone.
#
# Requires glslc (brew install shaderc) and spirv-val (brew install spirv-tools).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${root}/src/ui/shaders/source_d3d12"
out="${root}/src/ui/shaders/vulkan_spirv"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

for tool in glslc spirv-val; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "error: ${tool} not found on PATH" >&2
    exit 1
  }
done

# name : source : extra defines
build() {
  local name="$1" source="$2"
  shift 2

  echo "  ${name}"
  glslc -x hlsl \
        -fshader-stage=frag \
        -fentry-point=main \
        --target-env=vulkan1.0 \
        -O \
        -DXE_VULKAN=1 \
        "$@" \
        "${src}/${source}" \
        -o "${work}/${name}.spv"

  spirv-val "${work}/${name}.spv"

  python3 - "${work}/${name}.spv" "${out}/${name}.h" "${name}" <<'PY'
import struct
import sys

spv_path, header_path, name = sys.argv[1], sys.argv[2], sys.argv[3]
with open(spv_path, 'rb') as handle:
    blob = handle.read()

assert len(blob) % 4 == 0, 'SPIR-V blob is not word-aligned'
words = struct.unpack('<%dI' % (len(blob) // 4), blob)
assert words[0] == 0x07230203, 'not a little-endian SPIR-V module'

# These headers are #included inside `namespace shaders { ... }`, so they must
# not carry includes of their own and must match the upstream declaration style.
lines = ['// Generated with scripts/build_vulkan_shaders.sh - do not edit.',
         'const uint32_t %s[] = {' % name]
for start in range(0, len(words), 8):
    chunk = words[start:start + 8]
    lines.append('    ' + ' '.join('0x%08X,' % word for word in chunk))
lines.append('};')
lines.append('')

with open(header_path, 'w') as handle:
    handle.write('\n'.join(lines))
PY
}

echo "Building Vulkan SPIR-V for the Downpour guest-output effects:"
build guest_output_colour_grade_ps guest_output_colour_grade_ps.hlsl
build guest_output_box_ps          guest_output_box_ps.hlsl
build guest_output_box_dither_ps   guest_output_box_ps.hlsl -DXE_BOX_DITHER=1
echo "Done. Headers written to src/ui/shaders/vulkan_spirv/"
