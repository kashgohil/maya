#!/bin/sh
# Fetches the glTF sample models the glTF, animation, and R1 tests read (#1030, #1038, #1040), from
# KhronosGroup/glTF-Sample-Assets at a pinned commit, and one Poly Haven HDRI by its hash, into a folder
# outside the repository (default: build/render-samples). The models keep their own licenses (each
# folder's README and LICENSE.md); none of them is committed here.
# Usage: tools/fetch_render_samples.sh [folder]
set -eu
folder=${1:-build/render-samples}
commit=f36bfdabd1031c3cf6689a50570b8cdf3678b49c
models="Sponza DamagedHelmet FlightHelmet CesiumMan MetalRoughSpheres TextureTransformTest NormalTangentMirrorTest BoxTextured ABeautifulGame Fox SimpleSkin RiggedSimple RiggedFigure RecursiveSkeletons InterpolationTest"
if [ ! -d "$folder/.git" ]; then
    git clone --quiet --filter=blob:none --no-checkout https://github.com/KhronosGroup/glTF-Sample-Assets.git "$folder"
fi
cd "$folder"
git sparse-checkout init --cone
paths=""
for model in $models; do paths="$paths Models/$model"; done
# shellcheck disable=SC2086
git sparse-checkout set $paths
git checkout --quiet "$commit"
echo "glTF-Sample-Assets $commit: $models in $folder"
# Poly Haven's Aerodynamics Workshop HDRI (CC0), at 2k, by its SHA-256.
hdri=hdri/aerodynamics_workshop_2k.hdr
mkdir -p hdri
if [ ! -f "$hdri" ]; then
    curl -sL --fail -o "$hdri" https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/aerodynamics_workshop_2k.hdr
fi
echo "a7a96d5216f5ab162aeae007e14c0fb90c9ad8e7cd504a4ab05323947b208f3a  $hdri" | shasum -a 256 -c --quiet
echo "Poly Haven aerodynamics_workshop_2k.hdr in $folder/hdri"
