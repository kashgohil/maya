// stb_image and stb_image_resize2, compiled once for texture cooking (docs/assets.md#textures).
// Decoding is limited to PNG, JPEG, and (since #1035, for environments) Radiance HDR, from memory only.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
