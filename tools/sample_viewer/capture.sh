#!/usr/bin/env bash
# Captures the Sample Viewer comparison's references (docs/import.md#the-sample-viewer-comparison): the
# Khronos glTF Sample Renderer, pinned in package.json, renders views.json's views of the Khronos samples
# under the sample project's workshop HDRI, in headless Chromium on this machine's GPU.
#   tools/sample_viewer/capture.sh [output folder, default build/sample-viewer/captures] [--sky]
# Needs Node.js 20+, network access the first time, and the samples (tools/fetch_render_samples.sh).
# Inspect the images before copying them to tests/references/sample-viewer.
set -euo pipefail
tool="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$tool/../.." && pwd)"
work="$repo/build/sample-viewer"
output="${1:-$work/captures}"
samples="${MAYA_RENDER_SAMPLES:-$repo/build/render-samples}"
[[ -d "$samples/Models" ]] || { echo "no samples in $samples; run tools/fetch_render_samples.sh" >&2; exit 1; }

mkdir -p "$work/site/assets/images"
cp "$tool/package.json" "$work/"
(cd "$work" && npm install --no-audit --no-fund --silent && npx playwright-core install chromium)
cp "$tool/page.js" "$tool/capture.mjs" "$tool/views.json" "$work/"
(cd "$work" && npx esbuild page.js --bundle --format=iife --outfile=site/page.bundle.js --log-level=warning)
cp "$tool/index.html" "$work/site/"
rm -rf "$work/site/libs" && cp -R "$work/node_modules/@khronosgroup/gltf-viewer/dist/libs" "$work/site/libs"
# The renderer loads its sheen table from a file (the views use no sheen); the GGX table it computes.
[[ -f "$work/site/assets/images/lut_sheen_E.png" ]] || curl -sfL -o "$work/site/assets/images/lut_sheen_E.png" \
    https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Renderer/main/assets/images/lut_sheen_E.png
ln -sfn "$samples/Models" "$work/site/Models"
cp "$repo/samples/basic_scene/assets/environments/aerodynamics_workshop_1k.hdr" "$work/site/workshop.hdr"
cd "$work" && node capture.mjs site "$output" "${@:2}"
