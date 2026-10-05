#pragma once
// Small glTF files made in tests (#1036): a buffer of binary data, embedded as a base64 data URI, that
// test JSON points into with buffer views and accessors; and a small file using most of what an import handles.

#include "png.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace maya::test {

inline std::string base64(const std::string& bytes) {
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto out = std::string{};
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint8_t b0 = uint8_t(bytes[i]), b1 = i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) : 0,
                      b2 = i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0;
        out += digits[b0 >> 2];
        out += digits[((b0 & 3) << 4) | (b1 >> 4)];
        out += i + 1 < bytes.size() ? digits[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < bytes.size() ? digits[b2 & 63] : '=';
    }
    return out;
}

/// Binary data and the buffer views into it. Each view starts 4-byte aligned.
struct GltfBuffer {
    std::string bytes;
    std::vector<std::string> views; // JSON objects

    template<class T> size_t view(const std::vector<T>& values, size_t stride = 0) {
        while (bytes.size() % 4 != 0) bytes.push_back('\0');
        const auto offset = bytes.size();
        bytes.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" +
                        std::to_string(values.size() * sizeof(T)) + (stride ? R"(,"byteStride":)" + std::to_string(stride) : "") + "}");
        return views.size() - 1;
    }
    /// `"buffers":[...],"bufferViews":[...]`, to splice into a file's JSON.
    std::string json() const {
        auto text = R"("buffers":[{"byteLength":)" + std::to_string(bytes.size()) +
                    R"(,"uri":"data:application/octet-stream;base64,)" + base64(bytes) + R"("}],"bufferViews":[)";
        for (size_t i = 0; i < views.size(); ++i) text += (i ? "," : "") + views[i];
        return text + "]";
    }
};

/// A small file with most of what an import handles: a mesh with two primitives and two materials, a
/// mesh without a material, an embedded PNG and one beside the file, a texture transform, a light, a
/// camera, and a hierarchy. Its quads are `size` metres square. Its second image is textures/normal.png, beside it (flat_normal_png).
inline std::string props_gltf(const std::string& extra_root = "", float size = 1) {
    auto buffer = GltfBuffer{};
    const auto positions = buffer.view(std::vector<float>{0, 0, 0, size, 0, 0, size, size, 0, 0, size, 0});
    const auto normals = buffer.view(std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    const auto uvs = buffer.view(std::vector<float>{0, 1, 1, 1, 1, 0, 0, 0});
    const auto indices = buffer.view(std::vector<uint16_t>{0, 1, 2, 0, 2, 3});
    const auto png = encode_png_rgba(2, 2, std::vector<uint8_t>(16, 200));
    const auto primitive = [](const std::string& material) {
        return R"({"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3)" + material + "}";
    };
    return R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_lights_punctual","KHR_texture_transform"],)" + buffer.json() +
        R"(,"accessors":[
        {"bufferView":)" + std::to_string(positions) + R"(,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[)" + std::to_string(size) + "," + std::to_string(size) + R"(,0]},
        {"bufferView":)" + std::to_string(normals) + R"(,"componentType":5126,"count":4,"type":"VEC3"},
        {"bufferView":)" + std::to_string(uvs) + R"(,"componentType":5126,"count":4,"type":"VEC2"},
        {"bufferView":)" + std::to_string(indices) + R"(,"componentType":5123,"count":6,"type":"SCALAR"}],
        "images":[{"uri":"data:image/png;base64,)" + base64(png) + R"("},{"uri":"textures/normal.png"}],
        "textures":[{"source":0},{"source":1}],
        "materials":[
          {"name":"Painted","pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,1],"metallicFactor":0,
             "baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.5,0],"scale":[2,2]}}}},
           "normalTexture":{"index":1}},
          {"name":"Glow","emissiveTexture":{"index":0},"emissiveFactor":[1,1,1]}],
        "meshes":[{"name":"Panel","primitives":[)" + primitive(R"(,"material":0)") + "," + primitive(R"(,"material":1)") + R"(]},
                  {"name":"Tile","primitives":[)" + primitive("") + R"(]}],
        "extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","intensity":10,"range":5}]}},
        "cameras":[{"type":"perspective","perspective":{"yfov":0.7,"znear":0.1,"zfar":100}}],
        "nodes":[{"name":"Root","children":[1,2,3]},
                 {"name":"Panel","mesh":0,"translation":[0,1,0],"children":[4]},
                 {"name":"Lamp","translation":[0,3,0],"extensions":{"KHR_lights_punctual":{"light":0}}},
                 {"name":"Eye","camera":0,"translation":[0,1,5]},
                 {"name":"Tile","mesh":1}],
        "scenes":[{"nodes":[0]}],"scene":0)" + extra_root + "}";
}
/// A 2x2 normal map pointing straight out of the surface.
inline std::string flat_normal_png() {
    return encode_png_rgba(2, 2, {128, 128, 255, 255, 128, 128, 255, 255, 128, 128, 255, 255, 128, 128, 255, 255});
}

} // namespace maya::test
