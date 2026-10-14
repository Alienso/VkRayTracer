//
// glTF 2.0 loading with cgltf.
//
// A glTF file is a tree of nodes. Each node has a transform and may point at a mesh; a mesh is a list of
// primitives (triangles + one material). We turn every primitive into one Mesh and every node that uses
// a mesh into instances, with the node's transform multiplied down from its parents (EXT_mesh_gpu_instancing: one
// per entry of the node's instance arrays). Nodes with a light (KHR_lights_punctual) become PunctualLights the same way.
// Materials and the images they use are loaded along the way; images are decoded with stb_image.
//

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>
#include <stb_image.h>

#include "assets/ModelLoader.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <cstring>
#include <atomic>
#include <thread>

static const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < primitive.attributes_count; i++) {
        // index 0 = the first set (glTF allows TEXCOORD_0, TEXCOORD_1, ...)
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
            return primitive.attributes[i].data;
        }
    }
    return nullptr;
}

// Reads a whole accessor as floats, converting from whatever the file stores (normalized ints, sparse, ...).
static std::vector<float> readFloats(const cgltf_accessor* accessor) {
    std::vector<float> values(accessor->count * cgltf_num_components(accessor->type));
    cgltf_accessor_unpack_floats(accessor, values.data(), values.size());
    return values;
}

// ---------------------------------------------------------------------------------------------
// Textures

static TextureWrap toWrap(cgltf_wrap_mode mode) {
    switch (mode) {
        case cgltf_wrap_mode_clamp_to_edge: return TextureWrap::ClampToEdge;
        case cgltf_wrap_mode_mirrored_repeat: return TextureWrap::MirroredRepeat;
        default: return TextureWrap::Repeat;
    }
}

// DDS (DirectDraw Surface): a 128-byte header, for newer formats a 20-byte "DX10" header after it, then the
// mip levels one after the other. We keep the blocks as they are, with as many levels as the file has (at least
// the first). Returns false if the file isn't a DDS in one of the formats we can sample.
static bool readDds(const uint8_t* data, size_t size, TextureData& texture, std::string& error) {
    auto read32 = [&](size_t offset) {
        uint32_t value;
        std::memcpy(&value, data + offset, 4);
        return value;
    };
    if (size < 128 || std::memcmp(data, "DDS ", 4) != 0) {
        error = "not a DDS file";
        return false;
    }
    texture.height = read32(12);
    texture.width = read32(16);
    size_t offset = 128;
    char fourCC[5] = {};
    std::memcpy(fourCC, data + 84, 4);
    std::string format = fourCC;
    if (format == "DX10") {
        if (size < 148) {
            error = "truncated DDS header";
            return false;
        }
        switch (read32(128)) {  // DXGI_FORMAT, typeless / UNORM / UNORM_SRGB
            case 70: case 71: case 72: format = "DXT1"; break;
            case 73: case 74: case 75: format = "DXT3"; break;
            case 76: case 77: case 78: format = "DXT5"; break;
            case 82: case 83: format = "ATI2"; break;
            case 97: case 98: case 99: format = "BC7"; break;
            default: format = "DXGI " + std::to_string(read32(128));
        }
        offset = 148;
    }
    uint32_t blockBytes = 16;
    if (format == "DXT1") {
        texture.format = TextureFormat::BC1;
        blockBytes = 8;
    } else if (format == "DXT2" || format == "DXT3") {
        texture.format = TextureFormat::BC2;
    } else if (format == "DXT4" || format == "DXT5") {
        texture.format = TextureFormat::BC3;
    } else if (format == "ATI2" || format == "BC5U") {
        texture.format = TextureFormat::BC5;
    } else if (format == "BC7") {
        texture.format = TextureFormat::BC7;
    } else {
        error = "DDS format " + format + " not supported";
        return false;
    }
    auto levelSize = [&](uint32_t level) {
        uint32_t w = std::max(texture.width >> level, 1u), h = std::max(texture.height >> level, 1u);
        return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
    };
    if (texture.width == 0 || texture.height == 0 || offset + levelSize(0) > size) {
        error = "truncated DDS file";
        return false;
    }
    // The header's level count (0 or missing = 1); a level that doesn't fit in the file ends the chain
    uint32_t fileLevels = std::max(read32(28), 1u);
    uint32_t fullChain = 1;
    while ((std::max(texture.width, texture.height) >> fullChain) > 0) {
        fullChain++;
    }
    size_t end = offset;
    texture.mipLevels = 0;
    while (texture.mipLevels < std::min(fileLevels, fullChain) && end + levelSize(texture.mipLevels) <= size) {
        end += levelSize(texture.mipLevels);
        texture.mipLevels++;
    }
    texture.pixels.assign(data + offset, data + end);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Normal map filtering (Toksvig)
//
// A smaller mip level of a normal map averages its bumps away: from afar a bumpy surface would reflect as sharply as
// a polished one (and sparkle), although its many small bumps together spread the reflection. The average of unit
// normals is shorter than 1, the shorter the more they differ (Toksvig 2005). Taken as a von Mises-Fisher
// distribution, length r gives the concentration kappa = r (3 - r^2) / (1 - r^2) (Banerjee et al. 2005). That would
// be a Beckmann lobe of alpha^2 = 2 / kappa; GGX's long tails spread more light at the same alpha, and against the
// full-size reference (mipmaps off) alpha^2 = 1 / kappa came closest: average brightness Intel Sponza +2.7 % ->
// -0.2 % (2 / kappa: -2.3 %), Sponza +0.2 % -> -0.2 %, Zero-Day +0.45 % -> +0.34 %. The shader adds it to the
// material's alpha^2. BC5 maps lose the
// length (the shader rebuilds Z) and 8-bit maps keep it only roughly, so it is computed here from the full-size
// level, once per normal map, and stored as a texture of its own: BC4, sqrt(alpha^2) in 0..1.

// One BC4 block (8 bytes: two end values and a 3-bit index per texel) -> 16 values 0..255
static void decodeBc4Block(const uint8_t* block, uint8_t values[16]) {
    int e0 = block[0], e1 = block[1];
    int palette[8] = {e0, e1};
    if (e0 > e1) {
        for (int k = 1; k <= 6; k++) {
            palette[k + 1] = ((7 - k) * e0 + k * e1 + 3) / 7;
        }
    } else {
        for (int k = 1; k <= 4; k++) {
            palette[k + 1] = ((5 - k) * e0 + k * e1 + 2) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    }
    for (int t = 0; t < 16; t++) {
        values[t] = static_cast<uint8_t>(palette[(bits >> (3 * t)) & 7]);
    }
}

// 16 values 0..1 -> one BC4 block: the largest and smallest as end values, each texel the nearest of the 8 between
static void encodeBc4Block(const float values[16], uint8_t* block) {
    int quantized[16];
    int lo = 255, hi = 0;
    for (int t = 0; t < 16; t++) {
        quantized[t] = static_cast<int>(std::clamp(values[t], 0.0f, 1.0f) * 255.0f + 0.5f);
        lo = std::min(lo, quantized[t]);
        hi = std::max(hi, quantized[t]);
    }
    block[0] = static_cast<uint8_t>(hi);
    block[1] = static_cast<uint8_t>(lo);
    uint64_t bits = 0;
    if (hi > lo) {  // otherwise all texels take index 0 (= hi)
        for (int t = 0; t < 16; t++) {
            // Position between hi (0) and lo (7), then the palette's order: 0 = hi, 1 = lo, 2..7 = in between
            int step = static_cast<int>(std::lround(7.0 * (hi - quantized[t]) / (hi - lo)));
            uint64_t index = step == 0 ? 0 : step == 7 ? 1 : step + 1;
            bits |= index << (3 * t);
        }
    }
    for (int i = 0; i < 6; i++) {
        block[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
    }
}

// Whether makeNormalVarianceTexture can work with this normal map: 8-bit RGBA or BC5 (normal maps come in those),
// larger than 1x1
static bool normalVarianceSupported(const TextureData& normals) {
    return (normals.format == TextureFormat::RGBA8 || normals.format == TextureFormat::BC5) &&
           (normals.width > 1 || normals.height > 1);
}

// The unit normal for each stored X, Y (0..255) pair. Z is rebuilt from them, as the shader does for BC5; 8-bit maps
// store it too, but rounded.
static const std::vector<glm::vec3>& unitNormalTable() {
    static const std::vector<glm::vec3> table = [] {
        std::vector<glm::vec3> normals(256 * 256);
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                glm::vec2 v = glm::vec2(x, y) / 127.5f - 1.0f;
                normals[y * 256 + x] = glm::normalize(glm::vec3(v, std::sqrt(std::max(1.0f - glm::dot(v, v), 0.0f))));
            }
        }
        return normals;
    }();
    return table;
}

// Makes the normal map's Toksvig texture (see above and Material::normalVarianceTexture); normalVarianceSupported
// must hold. Several run at once (TextureLoader::finish).
static void makeNormalVarianceTexture(const TextureData& normals, TextureData& variance) {
    uint32_t width = normals.width, height = normals.height;
    const std::vector<glm::vec3>& table = unitNormalTable();

    // Averages of the unit normals, level by level (each the mean of the 2x2 below it; the last row / column of an
    // odd size is dropped, as in generateMipLevels), and per level the spread they leave out. Level 1 comes from the
    // full size, read two rows at a time: as stored, or from BC5 a block row (4 rows) decoded at a time.
    uint32_t levelWidth = std::max(width / 2, 1u), levelHeight = std::max(height / 2, 1u);
    std::vector<glm::vec3> average(static_cast<size_t>(levelWidth) * levelHeight);
    std::vector<uint8_t> blockRow(static_cast<size_t>(width) * 4 * 2);  // BC5: X, Y of 4 rows
    uint32_t decodedBlockRow = UINT32_MAX;
    for (uint32_t y = 0; y < levelHeight; y++) {
        const uint8_t* rows[2];
        uint32_t stride = 4;
        for (uint32_t d = 0; d < 2; d++) {
            uint32_t sy = std::min(y * 2 + d, height - 1);
            if (normals.format == TextureFormat::RGBA8) {
                rows[d] = &normals.pixels[static_cast<size_t>(sy) * width * 4];
                continue;
            }
            stride = 2;
            if (sy / 4 != decodedBlockRow) {
                decodedBlockRow = sy / 4;
                uint32_t blocksX = (width + 3) / 4;
                for (uint32_t bx = 0; bx < blocksX; bx++) {
                    const uint8_t* block = &normals.pixels[(static_cast<size_t>(decodedBlockRow) * blocksX + bx) * 16];
                    uint8_t x[16], yv[16];
                    decodeBc4Block(block, x);
                    decodeBc4Block(block + 8, yv);
                    for (uint32_t t = 0; t < 16; t++) {
                        uint32_t px = bx * 4 + t % 4;
                        if (px < width) {
                            size_t i = (static_cast<size_t>(t / 4) * width + px) * 2;
                            blockRow[i] = x[t];
                            blockRow[i + 1] = yv[t];
                        }
                    }
                }
            }
            rows[d] = &blockRow[static_cast<size_t>(sy % 4) * width * 2];
        }
        for (uint32_t x = 0; x < levelWidth; x++) {
            glm::vec3 sum(0.0f);
            for (uint32_t d = 0; d < 4; d++) {
                const uint8_t* texel = rows[d / 2] + static_cast<size_t>(std::min(x * 2 + d % 2, width - 1)) * stride;
                sum += table[texel[1] * 256 + texel[0]];
            }
            average[static_cast<size_t>(y) * levelWidth + x] = sum * 0.25f;
        }
    }
    variance.name = normals.name + " (Toksvig)";
    variance.width = levelWidth;
    variance.height = levelHeight;
    variance.format = TextureFormat::BC4;
    variance.mipLevels = 0;
    variance.nearest = normals.nearest;
    variance.wrapU = normals.wrapU;
    variance.wrapV = normals.wrapV;
    while (true) {
        // The level's spread, as 4x4 blocks
        uint32_t blocksX = (levelWidth + 3) / 4, blocksY = (levelHeight + 3) / 4;
        size_t start = variance.pixels.size();
        variance.pixels.resize(start + static_cast<size_t>(blocksX) * blocksY * 8);
        for (uint32_t by = 0; by < blocksY; by++) {
            for (uint32_t bx = 0; bx < blocksX; bx++) {
                float values[16];
                for (uint32_t t = 0; t < 16; t++) {
                    // Texels past the edge repeat the last row / column
                    uint32_t px = std::min(bx * 4 + t % 4, levelWidth - 1), py = std::min(by * 4 + t / 4, levelHeight - 1);
                    float r = std::clamp(glm::length(average[static_cast<size_t>(py) * levelWidth + px]), 1e-4f, 1.0f);
                    float alphaSquared = (1.0f - r * r) / (r * (3.0f - r * r));  // 1 / kappa
                    values[t] = std::sqrt(alphaSquared);
                }
                encodeBc4Block(values, &variance.pixels[start + (static_cast<size_t>(by) * blocksX + bx) * 8]);
            }
        }
        variance.mipLevels++;
        if (levelWidth == 1 && levelHeight == 1) {
            break;
        }
        uint32_t nextWidth = std::max(levelWidth / 2, 1u), nextHeight = std::max(levelHeight / 2, 1u);
        std::vector<glm::vec3> next(static_cast<size_t>(nextWidth) * nextHeight);
        for (uint32_t y = 0; y < nextHeight; y++) {
            for (uint32_t x = 0; x < nextWidth; x++) {
                glm::vec3 sum(0.0f);
                for (uint32_t d = 0; d < 4; d++) {
                    uint32_t sx = std::min(x * 2 + d % 2, levelWidth - 1), sy = std::min(y * 2 + d / 2, levelHeight - 1);
                    sum += average[static_cast<size_t>(sy) * levelWidth + sx];
                }
                next[static_cast<size_t>(y) * nextWidth + x] = sum * 0.25f;
            }
        }
        average = std::move(next);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
    }
}

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    bytes.resize(read);
    return bytes;
}

// Images live either inside the file (.glb, or a base64 "data:" URI) or in a file next to the .gltf.
// PNG / JPEG / ... are decoded with stb_image; DDS (MSFT_texture_dds) stays block-compressed.
static TextureData decodeImage(const cgltf_image* image, const std::string& directory) {
    TextureData texture;
    texture.name = image->name ? image->name : (image->uri && std::strncmp(image->uri, "data:", 5) != 0 ? image->uri : "image");

    std::vector<uint8_t> fileBytes;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    void* base64Data = nullptr;
    if (image->buffer_view) {
        bytes = cgltf_buffer_view_data(image->buffer_view);
        size = image->buffer_view->size;
    } else if (image->uri && std::strncmp(image->uri, "data:", 5) == 0) {
        const char* base64 = std::strstr(image->uri, "base64,");
        if (base64) {
            base64 += 7;
            size_t length = std::strlen(base64);
            size_t padding = (length > 0 && base64[length - 1] == '=') + (length > 1 && base64[length - 2] == '=');
            size_t decodedSize = length / 4 * 3 - padding;
            cgltf_options options{};
            if (cgltf_load_buffer_base64(&options, decodedSize, base64, &base64Data) == cgltf_result_success) {
                bytes = static_cast<const uint8_t*>(base64Data);
                size = decodedSize;
            }
        }
    } else if (image->uri) {
        std::string uri = image->uri;
        uri.resize(cgltf_decode_uri(&uri[0]));  // "%20" -> " ", ...
        fileBytes = readFile(directory + uri);
        bytes = fileBytes.data();
        size = fileBytes.size();
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = nullptr;
    if (size >= 4 && std::memcmp(bytes, "DDS ", 4) == 0) {
        std::string error;
        bool ok = readDds(bytes, size, texture, error);
        free(base64Data);  // cgltf allocates with malloc by default
        if (!ok) {
            throw std::runtime_error("failed to load image '" + texture.name + "': " + error);
        }
        return texture;
    }
    if (size > 0) {
        pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
    }
    free(base64Data);

    if (!pixels) {
        throw std::runtime_error("failed to decode image '" + texture.name + "': " +
                                 (size == 0 ? "file missing or empty"
                                            : stbi_failure_reason() ? stbi_failure_reason() : "unknown format"));
    }
    texture.width = static_cast<uint32_t>(width);
    texture.height = static_cast<uint32_t>(height);
    texture.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return texture;
}

// KHR_texture_transform as a 2x3 matrix: translation * rotation * scale. Identity if the slot has none.
// The rotation's sign follows Khronos' sample renderer and test models (TextureTransformTest); the GLSL snippet in
// the extension's README has it the other way round.
static glm::mat3x2 uvTransform(const cgltf_texture_view& view) {
    if (!view.texture || !view.has_transform) {
        return glm::mat3x2(1.0f);
    }
    const cgltf_texture_transform& t = view.transform;
    float c = std::cos(t.rotation), s = std::sin(t.rotation);
    return glm::mat3x2(glm::vec2(c, -s) * t.scale[0],
                       glm::vec2(s, c) * t.scale[1],
                       glm::vec2(t.offset[0], t.offset[1]));
}

// Loads the texture a material slot points at (once per texture and color space). Returns -1 if there is none.
// The mip levels of an 8-bit RGBA image (files bring none), appended to texture.pixels: each texel the average of
// the 2x2 below it (the last row / column of an odd size is dropped, a slight shift that doesn't show). sRGB
// colors are averaged as light, i.e. linear: averaged as stored, a fine black-and-white pattern would turn darker
// than the grey it looks like from afar. Alpha is coverage, averaged as it is.
static void generateMipLevels(TextureData& texture) {
    if (texture.format != TextureFormat::RGBA8 || texture.mipLevels > 1) {
        return;
    }
    float toLinear[256];
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        toLinear[i] = texture.srgb ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c;
    }
    auto toStored = [&](float linear) {
        float c = texture.srgb ? (linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f)
                               : linear;
        return static_cast<uint8_t>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
    };

    uint32_t width = texture.width, height = texture.height;
    size_t source = 0;  // where the level being halved starts in pixels
    while (width > 1 || height > 1) {
        uint32_t newWidth = std::max(width / 2, 1u), newHeight = std::max(height / 2, 1u);
        size_t target = texture.pixels.size();
        texture.pixels.resize(target + static_cast<size_t>(newWidth) * newHeight * 4);
        for (uint32_t y = 0; y < newHeight; y++) {
            for (uint32_t x = 0; x < newWidth; x++) {
                float sum[4] = {};
                for (uint32_t dy = 0; dy < 2; dy++) {
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        uint32_t sx = std::min(x * 2 + dx, width - 1), sy = std::min(y * 2 + dy, height - 1);
                        const uint8_t* p = &texture.pixels[source + (static_cast<size_t>(sy) * width + sx) * 4];
                        for (int c = 0; c < 3; c++) {
                            sum[c] += toLinear[p[c]];
                        }
                        sum[3] += p[3] / 255.0f;
                    }
                }
                uint8_t* out = &texture.pixels[target + (static_cast<size_t>(y) * newWidth + x) * 4];
                for (int c = 0; c < 3; c++) {
                    out[c] = toStored(sum[c] * 0.25f);
                }
                out[3] = static_cast<uint8_t>(std::clamp(sum[3] * 0.25f * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
        source = target;
        width = newWidth;
        height = newHeight;
        texture.mipLevels++;
    }
}

class TextureLoader {
public:
    TextureLoader(const cgltf_data* data, std::string directory, Model& model)
            : data(data), directory(std::move(directory)), model(model) {}

    int load(const cgltf_texture_view& view, bool srgb, const char* slot, const std::string& materialName) {
        if (!view.texture) {
            return -1;
        }
        // MSFT_texture_dds: the DDS version of the image is preferred (its "source" may be the only one)
        const cgltf_image* image = ddsImage(*view.texture);
        if (!image) {
            image = view.texture->image;
        }
        if (!image) {
            std::cerr << model.name << ": " << materialName << " " << slot
                      << ": image format not supported (KTX2/WebP?), using the constant only" << std::endl;
            return -1;
        }
        // The transform may pick a different UV set than the slot itself
        int texcoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
        if (texcoord != 0) {
            std::cerr << model.name << ": " << materialName << " " << slot
                      << " uses a second UV set, which isn't supported; using the first one" << std::endl;
        }

        // The same image can be used as color (sRGB) by one material and as data (linear) by another.
        auto key = std::make_pair(view.texture - data->textures, srgb);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }

        TextureData texture = decodeImage(image, directory);
        texture.srgb = srgb;
        if (const cgltf_sampler* sampler = view.texture->sampler) {
            texture.nearest = sampler->mag_filter == cgltf_filter_type_nearest;
            texture.wrapU = toWrap(sampler->wrap_s);
            texture.wrapV = toWrap(sampler->wrap_t);
        }
        int index = static_cast<int>(model.textures.size());
        model.textures.push_back(std::move(texture));
        cache[key] = index;
        return index;
    }

    // The normal map's Toksvig texture (see makeNormalVarianceTexture), one per normal map; -1 if none. Only its
    // place is taken here: finish() makes them all at once.
    int loadNormalVariance(int normalTexture) {
        if (normalTexture < 0) {
            return -1;
        }
        auto it = varianceCache.find(normalTexture);
        if (it != varianceCache.end()) {
            return it->second;
        }
        int index = -1;
        if (normalVarianceSupported(model.textures[normalTexture])) {
            index = static_cast<int>(model.textures.size());
            model.textures.emplace_back();
            varianceJobs.emplace_back(normalTexture, index);
        }
        varianceCache[normalTexture] = index;
        return index;
    }

    // Makes the Toksvig textures, then the mip levels of the 8-bit textures, on all cores: a few seconds for a scene
    // with dozens of 4K normal maps otherwise. The texture list doesn't change meanwhile, each job writes its own
    // entry. (The Toksvig textures read the normal maps' full size only, so their mip levels can come after.)
    void finish() {
        forAllCores(varianceJobs.size(), [&](size_t job) {
            makeNormalVarianceTexture(model.textures[varianceJobs[job].first], model.textures[varianceJobs[job].second]);
        });
        varianceJobs.clear();
        forAllCores(model.textures.size(), [&](size_t texture) { generateMipLevels(model.textures[texture]); });
    }

private:
    // job(0), ..., job(count - 1), spread over the cores
    template <typename Job>
    static void forAllCores(size_t count, Job&& job) {
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t i = next++; i < count; i = next++) {
                job(i);
            }
        };
        std::vector<std::thread> threads;
        size_t threadCount = std::min<size_t>(std::max(std::thread::hardware_concurrency(), 1u), count);
        for (size_t i = 1; i < threadCount; i++) {
            threads.emplace_back(work);
        }
        work();
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    // cgltf keeps extensions it doesn't know as JSON text, e.g. {"source":3}
    const cgltf_image* ddsImage(const cgltf_texture& texture) const {
        for (cgltf_size i = 0; i < texture.extensions_count; i++) {
            const cgltf_extension& extension = texture.extensions[i];
            if (!extension.name || !extension.data || std::strcmp(extension.name, "MSFT_texture_dds") != 0) {
                continue;
            }
            const char* source = std::strstr(extension.data, "\"source\"");
            const char* colon = source ? std::strchr(source, ':') : nullptr;
            if (colon) {
                long index = std::strtol(colon + 1, nullptr, 10);
                if (index >= 0 && static_cast<cgltf_size>(index) < data->images_count) {
                    return &data->images[index];
                }
            }
        }
        return nullptr;
    }

    const cgltf_data* data;
    std::string directory;
    Model& model;
    std::map<std::pair<ptrdiff_t, bool>, int> cache;
    std::map<int, int> varianceCache;  // normal map -> its Toksvig texture
    std::vector<std::pair<int, int>> varianceJobs;  // Toksvig textures to make: normal map, its place
};

// ---------------------------------------------------------------------------------------------
// Materials

static Material loadMaterial(const cgltf_material& gltfMaterial, TextureLoader& textures) {
    Material material;
    material.name = gltfMaterial.name ? gltfMaterial.name : "material";

    // glTF's own defaults (when a value is missing) are white, fully metallic, fully rough; cgltf fills those in.
    if (gltfMaterial.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = gltfMaterial.pbr_metallic_roughness;
        material.baseColor = glm::make_vec4(pbr.base_color_factor);
        material.metallic = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        material.baseColorTexture = textures.load(pbr.base_color_texture, true, "base color", material.name);
        material.metallicRoughnessTexture = textures.load(pbr.metallic_roughness_texture, false, "metallic/roughness", material.name);
        material.baseColorUvTransform = uvTransform(pbr.base_color_texture);
        material.metallicRoughnessUvTransform = uvTransform(pbr.metallic_roughness_texture);
    } else if (gltfMaterial.has_pbr_specular_glossiness) {
        // The old specular/glossiness workflow: at least keep the color.
        material.baseColor = glm::make_vec4(gltfMaterial.pbr_specular_glossiness.diffuse_factor);
        material.baseColorTexture = textures.load(gltfMaterial.pbr_specular_glossiness.diffuse_texture, true, "diffuse", material.name);
        material.baseColorUvTransform = uvTransform(gltfMaterial.pbr_specular_glossiness.diffuse_texture);
    }

    if (gltfMaterial.has_transmission) {
        material.transmission = gltfMaterial.transmission.transmission_factor;
        material.transmissionTexture = textures.load(gltfMaterial.transmission.transmission_texture, false, "transmission", material.name);
        material.transmissionUvTransform = uvTransform(gltfMaterial.transmission.transmission_texture);
    }
    if (gltfMaterial.has_ior) {
        material.ior = gltfMaterial.ior.ior;
    }
    if (gltfMaterial.has_volume) {
        // The thickness itself (for faking volume on rasterizers) isn't needed: we trace through the real mesh.
        material.solid = gltfMaterial.volume.thickness_factor > 0.0f;
        material.attenuationColor = glm::make_vec3(gltfMaterial.volume.attenuation_color);
        float distance = gltfMaterial.volume.attenuation_distance;  // cgltf: FLT_MAX when not given
        material.attenuationDistance = distance < 1e30f ? distance : 0.0f;
    }

    if (gltfMaterial.has_diffuse_transmission) {
        const cgltf_diffuse_transmission& dt = gltfMaterial.diffuse_transmission;
        material.diffuseTransmission = dt.diffuse_transmission_factor;
        material.diffuseTransmissionColor = glm::make_vec3(dt.diffuse_transmission_color_factor);
        material.diffuseTransmissionTexture = textures.load(dt.diffuse_transmission_texture, false, "diffuse transmission", material.name);
        material.diffuseTransmissionColorTexture = textures.load(dt.diffuse_transmission_color_texture, true, "diffuse transmission color", material.name);
        material.diffuseTransmissionUvTransform = uvTransform(dt.diffuse_transmission_texture);
        material.diffuseTransmissionColorUvTransform = uvTransform(dt.diffuse_transmission_color_texture);
    }

    material.emissiveColor = glm::make_vec3(gltfMaterial.emissive_factor);
    if (gltfMaterial.has_emissive_strength) {
        material.emissiveStrength = gltfMaterial.emissive_strength.emissive_strength;
    }
    material.emissiveTexture = textures.load(gltfMaterial.emissive_texture, true, "emissive", material.name);
    material.emissiveUvTransform = uvTransform(gltfMaterial.emissive_texture);

    material.normalTexture = textures.load(gltfMaterial.normal_texture, false, "normal", material.name);
    material.normalScale = gltfMaterial.normal_texture.scale;
    material.normalVarianceTexture = textures.loadNormalVariance(material.normalTexture);
    material.normalUvTransform = uvTransform(gltfMaterial.normal_texture);

    switch (gltfMaterial.alpha_mode) {
        case cgltf_alpha_mode_mask: material.alphaMode = AlphaMode::Mask; break;
        case cgltf_alpha_mode_blend: material.alphaMode = AlphaMode::Blend; break;
        default: material.alphaMode = AlphaMode::Opaque; break;
    }
    material.alphaCutoff = gltfMaterial.alpha_cutoff;
    return material;
}

// ---------------------------------------------------------------------------------------------
// Meshes

// Tangents for files that have a normal map but no TANGENT attribute.
// A normal map stores directions relative to the surface: X along the texture's U axis, Y along its V axis.
// So per triangle we work out which 3D direction U (and V) grow in, from how the UVs change along the edges,
// and average that over the triangles around each vertex.
// (Exporters use the more elaborate MikkTSpace algorithm; this simple version agrees with it on smooth meshes.)
static void generateTangents(Mesh& mesh) {
    std::vector<glm::vec3> uDirections(mesh.vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> vDirections(mesh.vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const SceneVertex& v0 = mesh.vertices[i0];
        const SceneVertex& v1 = mesh.vertices[i1];
        const SceneVertex& v2 = mesh.vertices[i2];

        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        // glTF's V axis points down the image, but a normal map's +Y means "up in the image". Flipping V here
        // makes the bitangent point up, matching what MikkTSpace-generated tangents in glTF files do.
        glm::vec2 duv1 = (v1.uv - v0.uv) * glm::vec2(1.0f, -1.0f);
        glm::vec2 duv2 = (v2.uv - v0.uv) * glm::vec2(1.0f, -1.0f);

        float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1e-12f) {
            continue;  // degenerate UVs, this triangle can't tell us anything
        }
        float r = 1.0f / determinant;
        glm::vec3 uDirection = (edge1 * duv2.y - edge2 * duv1.y) * r;
        glm::vec3 vDirection = (edge2 * duv1.x - edge1 * duv2.x) * r;
        for (uint32_t index : {i0, i1, i2}) {
            uDirections[index] += uDirection;
            vDirections[index] += vDirection;
        }
    }

    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        SceneVertex& vertex = mesh.vertices[i];
        glm::vec3 n = vertex.normal;
        // Make the tangent perpendicular to the normal (Gram-Schmidt)
        glm::vec3 t = uDirections[i] - n * glm::dot(n, uDirections[i]);
        if (glm::dot(t, t) < 1e-20f) {
            // No usable UVs around this vertex: any direction perpendicular to the normal will do
            t = glm::cross(std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f), n);
        }
        t = glm::normalize(t);
        // Mirrored UVs: cross(normal, tangent) points opposite to the V direction, then w = -1
        float handedness = glm::dot(glm::cross(n, t), vDirections[i]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = glm::vec4(t, handedness);
    }
    mesh.hasTangents = true;
}

static Mesh loadPrimitive(const cgltf_mesh& gltfMesh, const cgltf_primitive& primitive, size_t primitiveIndex) {
    Mesh mesh;
    mesh.name = gltfMesh.name ? gltfMesh.name : "mesh";
    if (gltfMesh.primitives_count > 1) {
        mesh.name += " #" + std::to_string(primitiveIndex);
    }

    const cgltf_accessor* positions = findAttribute(primitive, cgltf_attribute_type_position);
    const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* uvs = findAttribute(primitive, cgltf_attribute_type_texcoord);
    const cgltf_accessor* tangents = findAttribute(primitive, cgltf_attribute_type_tangent);

    std::vector<float> positionData = readFloats(positions);
    std::vector<float> normalData = normals ? readFloats(normals) : std::vector<float>();
    std::vector<float> uvData = uvs ? readFloats(uvs) : std::vector<float>();
    std::vector<float> tangentData = tangents ? readFloats(tangents) : std::vector<float>();
    mesh.hasNormals = normals != nullptr;
    mesh.hasTangents = tangents != nullptr && normals != nullptr;

    mesh.vertices.resize(positions->count);
    for (size_t i = 0; i < positions->count; i++) {
        SceneVertex& vertex = mesh.vertices[i];
        vertex.position = glm::make_vec3(&positionData[i * 3]);
        vertex.normal = mesh.hasNormals ? glm::make_vec3(&normalData[i * 3]) : glm::vec3(0.0f);
        vertex.uv = uvs ? glm::make_vec2(&uvData[i * 2]) : glm::vec2(0.0f);
        vertex.tangent = mesh.hasTangents ? glm::make_vec4(&tangentData[i * 4]) : glm::vec4(0.0f);
    }

    // Non-indexed primitives: every 3 consecutive vertices form a triangle.
    if (primitive.indices) {
        mesh.indices.resize(primitive.indices->count);
        cgltf_accessor_unpack_indices(primitive.indices, mesh.indices.data(), sizeof(uint32_t), mesh.indices.size());
    } else {
        mesh.indices.resize(positions->count);
        for (uint32_t i = 0; i < mesh.indices.size(); i++) {
            mesh.indices[i] = i;
        }
    }
    mesh.indices.resize(mesh.indices.size() / 3 * 3);  // drop a broken trailing triangle, if any

    bool hasNormalMap = primitive.material && primitive.material->normal_texture.texture;
    if (hasNormalMap && !mesh.hasTangents && normals && uvs) {
        generateTangents(mesh);
    }

    // glTF positions accessors are required to carry min/max, but don't trust that.
    mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const SceneVertex& vertex : mesh.vertices) {
        mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
        mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
    }
    return mesh;
}

// Texture coordinate -> texel index along one axis, following the texture's wrap mode
static uint32_t wrapTexel(float coordinate, uint32_t size, TextureWrap wrap) {
    float x = coordinate * static_cast<float>(size);
    int texel = static_cast<int>(std::floor(x));
    int n = static_cast<int>(size);
    switch (wrap) {
        case TextureWrap::ClampToEdge:
            texel = std::clamp(texel, 0, n - 1);
            break;
        case TextureWrap::MirroredRepeat: {
            int period = ((texel % (2 * n)) + 2 * n) % (2 * n);
            texel = period < n ? period : 2 * n - 1 - period;
            break;
        }
        default:
            texel = ((texel % n) + n) % n;
    }
    return static_cast<uint32_t>(texel);
}

static float srgbToLinear(uint8_t value) {
    float c = value / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// For light sampling: the average emissive texture color over every triangle, while the pixels are still on
// the CPU (they are dropped after the upload to the GPU). Samples a regular grid of points inside the triangle's
// UV area: about one per texel it covers, between 1 and 256 per triangle.
static void computeEmissiveTextureAverages(Model& model) {
    for (Mesh& mesh : model.meshes) {
        const Material& material = model.materials[mesh.materialIndex];
        if (material.emissiveTexture < 0) {
            continue;
        }
        const TextureData& texture = model.textures[material.emissiveTexture];
        if (texture.pixels.empty() || texture.format != TextureFormat::RGBA8) {
            continue;  // compressed: not decoded on the CPU, light sampling uses the emissive constant alone
        }
        float toLinear[256];
        for (int i = 0; i < 256; i++) {
            toLinear[i] = texture.srgb ? srgbToLinear(static_cast<uint8_t>(i)) : i / 255.0f;
        }

        mesh.emissiveTextureAverage.resize(mesh.indices.size() / 3);
        const glm::mat3x2& transform = material.emissiveUvTransform;  // affine: the triangle stays a triangle
        for (size_t t = 0; t < mesh.emissiveTextureAverage.size(); t++) {
            glm::vec2 uv0 = transform * glm::vec3(mesh.vertices[mesh.indices[3 * t]].uv, 1.0f);
            glm::vec2 uv1 = transform * glm::vec3(mesh.vertices[mesh.indices[3 * t + 1]].uv, 1.0f);
            glm::vec2 uv2 = transform * glm::vec3(mesh.vertices[mesh.indices[3 * t + 2]].uv, 1.0f);
            glm::vec2 e1 = uv1 - uv0, e2 = uv2 - uv0;
            float texels = 0.5f * std::abs(e1.x * e2.y - e1.y * e2.x) * texture.width * texture.height;
            // k x k grid of small triangles = k^2 sample points (their centers)
            int k = std::clamp(static_cast<int>(std::ceil(std::sqrt(texels))), 1, 16);

            glm::vec3 sum(0.0f);
            int count = 0;
            for (int i = 0; i < k; i++) {
                for (int j = 0; i + j < k; j++) {
                    // the "upright" small triangle at (i, j), and the upside-down one next to it
                    for (int flip = 0; flip < 2; flip++) {
                        if (flip == 1 && i + j == k - 1) {
                            continue;
                        }
                        float a = (i + (flip ? 2.0f : 1.0f) / 3.0f) / k;
                        float b = (j + (flip ? 2.0f : 1.0f) / 3.0f) / k;
                        glm::vec2 uv = uv0 + e1 * a + e2 * b;
                        uint32_t x = wrapTexel(uv.x, texture.width, texture.wrapU);
                        uint32_t y = wrapTexel(uv.y, texture.height, texture.wrapV);
                        const uint8_t* pixel = &texture.pixels[(static_cast<size_t>(y) * texture.width + x) * 4];
                        sum += glm::vec3(toLinear[pixel[0]], toLinear[pixel[1]], toLinear[pixel[2]]);
                        count++;
                    }
                }
            }
            mesh.emissiveTextureAverage[t] = sum / static_cast<float>(count);
        }
    }
}

static void addNodeInstances(const cgltf_data* data, const cgltf_node* node, const glm::mat4& parentTransform,
                             const std::vector<std::vector<uint32_t>>& meshesOfGltfMesh, Model& model) {
    float local[16];
    cgltf_node_transform_local(node, local);  // column major, like glm
    glm::mat4 transform = parentTransform * glm::make_mat4(local);

    if (node->mesh && node->has_mesh_gpu_instancing) {
        // EXT_mesh_gpu_instancing: the mesh once per entry of the node's TRANSLATION / ROTATION / SCALE arrays (each
        // may be missing), placed by node transform * T * R * S. For scenes with many copies (vegetation).
        std::vector<float> translations, rotations, scales;
        size_t count = 0;
        for (cgltf_size i = 0; i < node->mesh_gpu_instancing.attributes_count; i++) {
            const cgltf_attribute& attribute = node->mesh_gpu_instancing.attributes[i];
            std::string name = attribute.name ? attribute.name : "";
            count = attribute.data->count;
            if (name == "TRANSLATION") {
                translations = readFloats(attribute.data);
            } else if (name == "ROTATION") {
                rotations = readFloats(attribute.data);  // x, y, z, w
            } else if (name == "SCALE") {
                scales = readFloats(attribute.data);
            }
        }
        size_t gltfMeshIndex = node->mesh - data->meshes;
        for (size_t k = 0; k < count; k++) {
            glm::mat4 instance(1.0f);
            if (!translations.empty()) {
                instance = glm::translate(instance, glm::make_vec3(&translations[k * 3]));
            }
            if (!rotations.empty()) {
                const float* q = &rotations[k * 4];
                instance *= glm::mat4_cast(glm::normalize(glm::quat(q[3], q[0], q[1], q[2])));  // glm: w first
            }
            if (!scales.empty()) {
                instance = glm::scale(instance, glm::make_vec3(&scales[k * 3]));
            }
            for (uint32_t meshIndex : meshesOfGltfMesh[gltfMeshIndex]) {
                model.instances.push_back({meshIndex, transform * instance});
            }
        }
    } else if (node->mesh) {
        size_t gltfMeshIndex = node->mesh - data->meshes;
        for (uint32_t meshIndex : meshesOfGltfMesh[gltfMeshIndex]) {
            model.instances.push_back({meshIndex, transform});
        }
    }
    if (node->light && node->light->type != cgltf_light_type_invalid) {
        const cgltf_light& gltfLight = *node->light;
        PunctualLight light;
        light.name = gltfLight.name ? gltfLight.name : (node->name ? node->name : "light");
        light.type = gltfLight.type == cgltf_light_type_directional ? LightType::Directional
                   : gltfLight.type == cgltf_light_type_spot ? LightType::Spot : LightType::Point;
        light.color = glm::make_vec3(gltfLight.color);
        light.intensity = gltfLight.intensity;
        light.range = gltfLight.range;
        if (light.type == LightType::Spot) {
            light.innerConeAngle = gltfLight.spot_inner_cone_angle;
            light.outerConeAngle = gltfLight.spot_outer_cone_angle;
        }
        light.transform = transform;
        model.lights.push_back(light);
    }
    for (cgltf_size i = 0; i < node->children_count; i++) {
        addNodeInstances(data, node->children[i], transform, meshesOfGltfMesh, model);
    }
}

Model loadGltfModel(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        throw std::runtime_error("failed to parse glTF file: " + path);
    }
    // .glb files carry their binary data inside, .gltf files point at separate .bin files.
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        throw std::runtime_error("failed to load glTF buffers: " + path);
    }

    Model model;
    size_t slash = path.find_last_of("/\\");
    model.name = path.substr(slash + 1);
    std::string directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // 1: materials (and the textures they use), in the file's order so primitives can refer to them by index.
    //    One extra default material at the end for primitives without one.
    try {
        TextureLoader textures(data, directory, model);
        for (cgltf_size m = 0; m < data->materials_count; m++) {
            model.materials.push_back(loadMaterial(data->materials[m], textures));
        }
        textures.finish();
    } catch (...) {
        cgltf_free(data);
        throw;
    }
    uint32_t defaultMaterial = static_cast<uint32_t>(model.materials.size());
    bool defaultMaterialUsed = false;

    // 2: meshes. One glTF mesh can turn into several of ours (one per primitive).
    std::vector<std::vector<uint32_t>> meshesOfGltfMesh(data->meshes_count);
    for (cgltf_size m = 0; m < data->meshes_count; m++) {
        const cgltf_mesh& gltfMesh = data->meshes[m];
        for (cgltf_size p = 0; p < gltfMesh.primitives_count; p++) {
            const cgltf_primitive& primitive = gltfMesh.primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;  // points and lines can't be ray traced as triangles
            }
            if (primitive.has_draco_mesh_compression || !findAttribute(primitive, cgltf_attribute_type_position)) {
                std::cerr << model.name << ": skipping a compressed or position-less primitive" << std::endl;
                continue;
            }
            Mesh mesh = loadPrimitive(gltfMesh, primitive, p);
            if (mesh.indices.empty()) {
                continue;
            }
            if (primitive.material) {
                mesh.materialIndex = static_cast<uint32_t>(primitive.material - data->materials);
            } else {
                mesh.materialIndex = defaultMaterial;
                defaultMaterialUsed = true;
            }
            meshesOfGltfMesh[m].push_back(static_cast<uint32_t>(model.meshes.size()));
            model.meshes.push_back(std::move(mesh));
        }
    }

    if (defaultMaterialUsed) {
        model.materials.emplace_back();
    }
    computeEmissiveTextureAverages(model);

    // 3: instances, by walking the node tree of the default scene (or all root nodes if there is none)
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; i++) {
            addNodeInstances(data, scene->nodes[i], glm::mat4(1.0f), meshesOfGltfMesh, model);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (!data->nodes[i].parent) {
                addNodeInstances(data, &data->nodes[i], glm::mat4(1.0f), meshesOfGltfMesh, model);
            }
        }
    }
    cgltf_free(data);

    if (model.instances.empty()) {
        throw std::runtime_error("glTF file contains no triangle meshes: " + path);
    }

    // 4: world space bounds: transform the 8 corners of every instance's box
    model.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    model.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const MeshInstance& instance : model.instances) {
        const Mesh& mesh = model.meshes[instance.meshIndex];
        for (int i = 0; i < 8; i++) {
            glm::vec3 corner = {(i & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                                (i & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                                (i & 4) ? mesh.boundsMax.z : mesh.boundsMin.z};
            glm::vec3 world = glm::vec3(instance.transform * glm::vec4(corner, 1.0f));
            model.boundsMin = glm::min(model.boundsMin, world);
            model.boundsMax = glm::max(model.boundsMax, world);
        }
    }
    return model;
}
