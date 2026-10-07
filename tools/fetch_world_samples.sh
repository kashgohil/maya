#!/bin/sh
# Fetches the W1 reference world's content (#1060, docs/architecture/world-scale-decision.md#w1-the-reference-world):
# six scatter models, two terrain textures, and a sky from Poly Haven (all CC0), at 2k, each checked
# by its SHA-256, into a folder outside the repository (default: build/world-samples). Nothing here is
# committed. R1's content, which W1's landmarks reuse, comes from tools/fetch_render_samples.sh.
# Usage: tools/fetch_world_samples.sh [folder]
set -eu
folder=${1:-build/world-samples}
mkdir -p "$folder"
cd "$folder"
fetched=0
# SHA-256, path in the folder, source.
while read -r sum path url; do
    [ -n "$sum" ] || continue
    if [ ! -f "$path" ] || ! echo "$sum  $path" | shasum -a 256 -c --quiet >/dev/null 2>&1; then
        mkdir -p "$(dirname "$path")"
        curl -sL --fail --retry 6 --retry-all-errors --retry-delay 3 -A maya-fetch-world-samples -o "$path" "$url"
        fetched=$((fetched + 1))
    fi
    echo "$sum  $path" | shasum -a 256 -c --quiet
done <<'MANIFEST'
52d118b56748a18509e4ec0075d07b254d72a0401793f22363c0bb3a682e7361 models/boulder_01/boulder_01_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/boulder_01/boulder_01_2k.gltf
5174b318712ac7725cbcb55d422607f9c66d7f8d035c5412ab897b5939e2db6b models/boulder_01/textures/boulder_01_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/boulder_01/boulder_01_nor_gl_2k.jpg
90bbaa17c1fe0254d2b0b6e5148264fa834953a72724d566f14af029eebe3ca3 models/boulder_01/textures/boulder_01_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/boulder_01/boulder_01_diff_2k.jpg
7f5b06503d62bd95bfe9c6b8a354a5cc8ff04a4d271a8cf87a3e3125db4232d7 models/boulder_01/boulder_01.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/boulder_01/boulder_01.bin
ab52859dde519b4822111aa60916b6d21f8a6966c67b6cec44d1cd0775b8a86a models/boulder_01/textures/boulder_01_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/boulder_01/boulder_01_arm_2k.jpg
e181b722c8c8bd56f98ab8883b283b3cb3c437c372af3b831aec2f94c22fe0f2 models/namaqualand_boulder_02/namaqualand_boulder_02_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/namaqualand_boulder_02/namaqualand_boulder_02_2k.gltf
0f6daa7ec9b1a92207c0b09b737ff1eec904aebc1c1eada778afa6e5eacd55c0 models/namaqualand_boulder_02/namaqualand_boulder_02.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/namaqualand_boulder_02/namaqualand_boulder_02.bin
0f81c51224deb025079f10dad21530e77e42baad541eb766922fe8fc6e46a88e models/namaqualand_boulder_02/textures/namaqualand_boulder_02_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/namaqualand_boulder_02/namaqualand_boulder_02_diff_2k.jpg
2314bc882e140adba4ee2eae2873f11458b359208b59240a04ae85f2e5766014 models/namaqualand_boulder_02/textures/namaqualand_boulder_02_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/namaqualand_boulder_02/namaqualand_boulder_02_arm_2k.jpg
3067767374241dec6612ad60cc806cb9e30594f784d74f19a7cda4cdc2d28cc0 models/namaqualand_boulder_02/textures/namaqualand_boulder_02_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/namaqualand_boulder_02/namaqualand_boulder_02_nor_gl_2k.jpg
794beaf2f4dc0ab155114f286049dc912a8f3287520965ca8bf6bb0d821ae3e2 models/rock_moss_set_01/rock_moss_set_01_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/rock_moss_set_01/rock_moss_set_01_2k.gltf
de8360e6175879aaf02c171911069cf5f1852547f9ca7f10e440085dbcc3315e models/rock_moss_set_01/rock_moss_set_01.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/rock_moss_set_01/rock_moss_set_01.bin
bdee6da8baba67bd7e85e414e669cd46aab9afaf755114fb5163f0e68775c3d6 models/rock_moss_set_01/textures/rock_moss_set_01_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_01/rock_moss_set_01_diff_2k.jpg
f11c238a203eebd5e82da3a50468040a78d25fd2e46783c4ad79b1022b144c0d models/rock_moss_set_01/textures/rock_moss_set_01_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_01/rock_moss_set_01_nor_gl_2k.jpg
a05a8bd9f2563cf55b738484b894671df7b2391aa02d66451366c5ad38077cd2 models/rock_moss_set_01/textures/rock_moss_set_01_rough_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_01/rock_moss_set_01_rough_2k.jpg
d17cf3d4ea94bdc5007b249cdc2f42c8525cf312043a90c6762361ef5c03fd4a models/rock_moss_set_02/rock_moss_set_02_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/rock_moss_set_02/rock_moss_set_02_2k.gltf
796cbab6ee2708f443f5a57e9081ac8747c703ab035e9cf3a438147bcde187f0 models/rock_moss_set_02/textures/rock_moss_set_02_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_02/rock_moss_set_02_diff_2k.jpg
2df9d8eaaca4207529b0f613d4886d3962d0b9ffc0cdfd2bc4bf35dbcd391cb5 models/rock_moss_set_02/rock_moss_set_02.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/rock_moss_set_02/rock_moss_set_02.bin
5d2d7095a4ad459b40d3da2c5d9083e0735484a7df6033ac696e4379fb2e86f3 models/rock_moss_set_02/textures/rock_moss_set_02_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_02/rock_moss_set_02_nor_gl_2k.jpg
89a178fc23de0c3df2f316a869bcd74a3c17bc646405a3ba66e4c6c51230a9e6 models/rock_moss_set_02/textures/rock_moss_set_02_rough_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/rock_moss_set_02/rock_moss_set_02_rough_2k.jpg
9fd1a672738ce1023318b9af829ccc725bb05ddcf38ff2de905a66f42d8f5143 models/tree_stump_01/tree_stump_01_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/tree_stump_01/tree_stump_01_2k.gltf
b4d9ca70b3c908c4a638fbd6a23ed96c84055bbbf14cf44b89e31ad59fa21e0a models/tree_stump_01/tree_stump_01.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/tree_stump_01/tree_stump_01.bin
2cf395f2779c9e3dfab96088af1384573ecc3f36edbd5ccae8dfe26577c969b0 models/tree_stump_01/textures/tree_stump_01_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/tree_stump_01/tree_stump_01_nor_gl_2k.jpg
77d5238bc40168bcff0833dcf6510aecea4a2b6746968fed7879c5ca7864e6f5 models/tree_stump_01/textures/tree_stump_01_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/tree_stump_01/tree_stump_01_diff_2k.jpg
b025cf566d4ce1da452353f3d98c6ddee2fa63b2b440cfa94d9405de347f846f models/tree_stump_01/textures/tree_stump_01_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/tree_stump_01/tree_stump_01_arm_2k.jpg
47699d11f00d22d5f87f8776f94c9f2516f839a0de9c5da80126b355200edd42 models/dead_tree_trunk_02/dead_tree_trunk_02_2k.gltf https://dl.polyhaven.org/file/ph-assets/Models/gltf/2k/dead_tree_trunk_02/dead_tree_trunk_02_2k.gltf
c01134451c6610e3c9225f16d8da2080b0eaba4df137e2bf1a7980d7ce417f35 models/dead_tree_trunk_02/textures/dead_tree_trunk_02_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/dead_tree_trunk_02/dead_tree_trunk_02_nor_gl_2k.jpg
d5b928c12885249e5a5fbe13619ac9693a67f6196137cc9eadedd5a6b90a87de models/dead_tree_trunk_02/textures/dead_tree_trunk_02_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/dead_tree_trunk_02/dead_tree_trunk_02_diff_2k.jpg
0be2e1a7c945090c385c7ae2636e6e6c341d4da14d741164b5cfe3080655331d models/dead_tree_trunk_02/textures/dead_tree_trunk_02_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Models/jpg/2k/dead_tree_trunk_02/dead_tree_trunk_02_arm_2k.jpg
2b1b7ca07d98396ddc1987e367dcff9734df16b62e997c14837512d134687ded models/dead_tree_trunk_02/dead_tree_trunk_02.bin https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/dead_tree_trunk_02/dead_tree_trunk_02.bin
24b8aaf4c8547305d80b0e029ec365219ab1b90748d4ca5651bdc9e75bfde5e6 textures/forest_ground_04/forest_ground_04_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/forest_ground_04/forest_ground_04_diff_2k.jpg
b37ac799ed410976541e0011c246fca254e74eaa3f096ae7a7d74178db5a6d9b textures/forest_ground_04/forest_ground_04_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/forest_ground_04/forest_ground_04_nor_gl_2k.jpg
a6086cb5611fabcc3dd4c9ac74bcf6cf8ed5d07e136ebe51255a5a3bea9e42c6 textures/forest_ground_04/forest_ground_04_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/forest_ground_04/forest_ground_04_arm_2k.jpg
644ee79df7ffbf5f6afc330b64dd3f33794bab8820915fe61ffe0c88ecfd7ece textures/rock_face_03/rock_face_03_diff_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/rock_face_03/rock_face_03_diff_2k.jpg
a498cef4ba6191f6959b6d03fb835263c77271948264a289af24497ab7111dec textures/rock_face_03/rock_face_03_nor_gl_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/rock_face_03/rock_face_03_nor_gl_2k.jpg
1921303981d12bccf72dd29d8a8daf823f1d9bb60400b447c64757203c1e2dad textures/rock_face_03/rock_face_03_arm_2k.jpg https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/rock_face_03/rock_face_03_arm_2k.jpg
5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198 hdri/kloofendal_48d_partly_cloudy_puresky_2k.hdr https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr
MANIFEST
echo "W1 content (Poly Haven, CC0) in $folder; $fetched files fetched"
