#include "lsd/asset/texture.h"

#include "lsd/foundation/log.h"

#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_dxt.h>
#include <tinyexr.h>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <cstring>

namespace lsd::asset {

namespace {

// sRGB byte -> linear, and back (a table each way: mips of big textures touch millions of pixels).
struct SrgbTables {
    f32 to_linear[256];
    u8 to_srgb[4096];  // indexed by linear * 4095
    SrgbTables() {
        for (int i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            to_linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < 4096; ++i) {
            const f32 l = static_cast<f32>(i) / 4095.0f;
            const f32 c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            to_srgb[i] = static_cast<u8>(c * 255.0f + 0.5f);
        }
    }
};
const SrgbTables& srgb_tables() {
    static const SrgbTables tables;
    return tables;
}

// One 2x2 box-filtered level from the one above (odd edges repeat their last row / column).
void downsample(const u8* src, u32 w, u32 h, u8* dst, bool srgb) {
    const u32 dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    const SrgbTables& t = srgb_tables();
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            const u32 y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            const u8* p[4] = {src + (y0 * w + x0) * 4, src + (y0 * w + x1) * 4, src + (y1 * w + x0) * 4, src + (y1 * w + x1) * 4};
            u8* out = dst + (y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                if (srgb && c < 3) {  // average light, not bytes: keeps colour mips from darkening
                    const f32 sum = t.to_linear[p[0][c]] + t.to_linear[p[1][c]] + t.to_linear[p[2][c]] + t.to_linear[p[3][c]];
                    out[c] = t.to_srgb[static_cast<int>(sum * 0.25f * 4095.0f + 0.5f)];
                } else {
                    out[c] = static_cast<u8>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                }
            }
        }
    }
}

} // namespace

bool build_texture(const u8* rgba, u32 width, u32 height, bool srgb, u32 max_size, TextureData& out) {
    if (!rgba || width == 0 || height == 0) return false;
    // Halve until it fits (the extra levels are simply not kept).
    Array<u8> scratch(default_allocator());
    scratch.resize(static_cast<usize>(width) * height * 4);
    std::memcpy(scratch.data(), rgba, scratch.size());
    max_size = max_size ? max_size : 1;
    while (width > max_size || height > max_size) {
        Array<u8> smaller(default_allocator());
        const u32 w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1;
        smaller.resize(static_cast<usize>(w) * h * 4);
        downsample(scratch.data(), width, height, smaller.data(), srgb);
        scratch.clear();
        scratch.resize(smaller.size());
        std::memcpy(scratch.data(), smaller.data(), smaller.size());
        width = w;
        height = h;
    }
    out.width = width;
    out.height = height;
    out.mip_count = 0;
    usize total = 0;
    for (u32 w = width, h = height;; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
        out.mip_offset[out.mip_count++] = total;
        total += static_cast<usize>(w) * h * 4;
        if ((w == 1 && h == 1) || out.mip_count == kMaxTextureMips) break;
    }
    out.pixels.resize(total);
    std::memcpy(out.pixels.data(), scratch.data(), scratch.size());
    u32 w = width, h = height;
    for (u32 m = 1; m < out.mip_count; ++m) {
        downsample(out.pixels.data() + out.mip_offset[m - 1], w, h, out.pixels.data() + out.mip_offset[m], srgb);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return true;
}

static bool is_float_image(const char* path, bool& exr) {
    const usize n = std::strlen(path);
    auto ends = [&](const char* ext) {
        const usize e = std::strlen(ext);
        if (n < e) return false;
        for (usize i = 0; i < e; ++i) {
            if (std::tolower(static_cast<unsigned char>(path[n - e + i])) != ext[i]) return false;
        }
        return true;
    };
    exr = ends(".exr");
    return exr || ends(".hdr");
}

// Float RGBA (linear) to bytes: sRGB-encoded for colour, as they are for data; clipped to 0..1.
static void float_to_rgba8(const f32* in, usize pixels, bool srgb, u8* out) {
    for (usize i = 0; i < pixels * 4; ++i) {
        const f32 v = in[i] != in[i] ? 0.0f : in[i] < 0.0f ? 0.0f : in[i] > 1.0f ? 1.0f : in[i];  // NaN: black
        out[i] = srgb && i % 4 != 3 ? srgb_tables().to_srgb[static_cast<u32>(v * 4095.0f + 0.5f)] : static_cast<u8>(v * 255.0f + 0.5f);
    }
}

bool load_texture(const char* path, bool srgb, u32 max_size, TextureData& out) {
    int w = 0, h = 0, channels = 0;
    if (bool exr; is_float_image(path, exr)) {
        f32* rgba = nullptr;
        const char* err = nullptr;
        if (exr) {
            if (LoadEXR(&rgba, &w, &h, path, &err) != TINYEXR_SUCCESS) {
                LSD_LOG_WARN("asset", "Could not read EXR %s: %s", path, err ? err : "unknown error");
                FreeEXRErrorMessage(err);
                return false;
            }
        } else {
            rgba = stbi_loadf(path, &w, &h, &channels, 4);
            if (!rgba) {
                LSD_LOG_WARN("asset", "Could not read HDR %s: %s", path, stbi_failure_reason());
                return false;
            }
        }
        Array<u8> bytes(out.pixels.allocator());
        bytes.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4);
        float_to_rgba8(rgba, static_cast<usize>(w) * static_cast<usize>(h), srgb, bytes.data());
        free(rgba);  // both stb and tinyexr allocate with malloc
        return build_texture(bytes.data(), static_cast<u32>(w), static_cast<u32>(h), srgb, max_size, out);
    }
    stbi_uc* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        LSD_LOG_WARN("asset", "Could not read image %s: %s", path, stbi_failure_reason());
        return false;
    }
    const bool ok = build_texture(pixels, static_cast<u32>(w), static_cast<u32>(h), srgb, max_size, out);
    stbi_image_free(pixels);
    return ok;
}

bool load_grey16(const char* path, u32& width, u32& height, Array<u16>& out) {
    int w = 0, h = 0, channels = 0;
    stbi_us* grey = stbi_load_16(path, &w, &h, &channels, 1);
    if (!grey) return false;
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
    out.resize(static_cast<usize>(w) * static_cast<usize>(h));
    std::memcpy(out.data(), grey, out.size() * sizeof(u16));
    stbi_image_free(grey);
    return true;
}

bool encode_png(const char* path, u32 max_side, Array<u8>& png) {
    TextureData image(png.allocator());
    if (!load_texture(path, false, max_side, image)) return false;  // halves it until it fits
    png.clear();
    auto append = [](void* context, void* data, int size) {
        auto& out = *static_cast<Array<u8>*>(context);
        const usize at = out.size();
        out.resize(at + static_cast<usize>(size));
        std::memcpy(out.data() + at, data, static_cast<usize>(size));
    };
    return stbi_write_png_to_func(append, &png, static_cast<int>(image.width), static_cast<int>(image.height), 4, image.mip(0), static_cast<int>(image.width * 4)) != 0;
}

bool compress_texture(TextureData& data) {
    if (data.encoding != TextureEncoding::RGBA8 || !data.mip_count || data.width % 4 || data.height % 4) return false;
    bool opaque = true;
    for (usize i = 3; i < static_cast<usize>(data.width) * data.height * 4 && opaque; i += 4) opaque = data.mip(0)[i] == 255;
    const usize block_bytes = opaque ? 8 : 16;
    Array<u8> blocks(data.pixels.allocator());
    usize offsets[kMaxTextureMips] = {};
    u32 w = data.width, h = data.height;
    for (u32 m = 0; m < data.mip_count; ++m) {
        const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
        offsets[m] = blocks.size();
        blocks.resize(blocks.size() + static_cast<usize>(bw) * bh * block_bytes);
        const u8* src = data.mip(m);
        for (u32 by = 0; by < bh; ++by) {
            for (u32 bx = 0; bx < bw; ++bx) {
                u8 block[64];  // 4x4 RGBA, edges repeated for mips smaller than a block
                for (u32 y = 0; y < 4; ++y) {
                    for (u32 x = 0; x < 4; ++x) {
                        const u32 sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                        std::memcpy(block + (y * 4 + x) * 4, src + (static_cast<usize>(sy) * w + sx) * 4, 4);
                    }
                }
                stb_compress_dxt_block(blocks.data() + offsets[m] + (static_cast<usize>(by) * bw + bx) * block_bytes, block, opaque ? 0 : 1, STB_DXT_HIGHQUAL);
            }
        }
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    data.pixels.resize(blocks.size());
    std::memcpy(data.pixels.data(), blocks.data(), blocks.size());
    for (u32 m = 0; m < data.mip_count; ++m) data.mip_offset[m] = offsets[m];
    data.encoding = opaque ? TextureEncoding::BC1 : TextureEncoding::BC3;
    return true;
}

namespace {
constexpr char kTextureCacheMagic[8] = {'L', 'S', 'D', 'T', 'E', 'X', '1', '\0'};
struct TextureCacheHeader {
    char magic[8];
    i64 source_time;
    u64 source_size;
    u32 max_size, width, height, mip_count;
    u32 encoding, srgb;
    u64 mip_offset[kMaxTextureMips];
    u64 bytes;
};
} // namespace

bool load_texture_cached(const char* path, bool srgb, u32 max_size, bool compress, const char* cache_dir, TextureData& out) {
    if (!compress || !cache_dir) return load_texture(path, srgb, max_size, out);
    namespace fs = std::filesystem;
    std::error_code ec;
    const i64 time = static_cast<i64>(fs::last_write_time(path, ec).time_since_epoch().count());
    const u64 size = ec ? 0 : static_cast<u64>(fs::file_size(path, ec));
    if (ec) return load_texture(path, srgb, max_size, out);
    u64 hash = 1469598103934665603ull;  // which cached file: the source's path, colour or data, the size cap
    for (const char* p = path; *p; ++p) hash = (hash ^ static_cast<u8>(*p == '\\' ? '/' : *p)) * 1099511628211ull;
    hash = (hash ^ (srgb ? 1u : 0u) ^ (static_cast<u64>(max_size) << 8)) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.lsdtex", static_cast<unsigned long long>(hash));
    const fs::path cached = fs::path(cache_dir) / name;
    if (FILE* f = std::fopen(cached.string().c_str(), "rb")) {  // still the same source: read it back
        TextureCacheHeader h{};
        const bool header = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kTextureCacheMagic, 8) == 0 && h.source_time == time &&
                            h.source_size == size && h.max_size == max_size && h.srgb == (srgb ? 1u : 0u) && h.mip_count && h.mip_count <= kMaxTextureMips;
        if (header) {
            out.pixels.resize(h.bytes);
            if (std::fread(out.pixels.data(), 1, h.bytes, f) == h.bytes) {
                std::fclose(f);
                out.width = h.width;
                out.height = h.height;
                out.mip_count = h.mip_count;
                out.encoding = static_cast<TextureEncoding>(h.encoding);
                for (u32 m = 0; m < kMaxTextureMips; ++m) out.mip_offset[m] = h.mip_offset[m];
                return true;
            }
        }
        std::fclose(f);
    }
    if (!load_texture(path, srgb, max_size, out)) return false;
    compress_texture(out);  // stays RGBA8 when its size is not a multiple of 4 (cached that way too)
    fs::create_directories(cache_dir, ec);
    if (FILE* f = std::fopen(cached.string().c_str(), "wb")) {  // a failed write only means encoding again next time
        TextureCacheHeader h{};
        std::memcpy(h.magic, kTextureCacheMagic, 8);
        h.source_time = time;
        h.source_size = size;
        h.max_size = max_size;
        h.width = out.width;
        h.height = out.height;
        h.mip_count = out.mip_count;
        h.encoding = static_cast<u32>(out.encoding);
        h.srgb = srgb ? 1u : 0u;
        for (u32 m = 0; m < kMaxTextureMips; ++m) h.mip_offset[m] = out.mip_offset[m];
        h.bytes = out.pixels.size();
        std::fwrite(&h, sizeof h, 1, f);
        std::fwrite(out.pixels.data(), 1, out.pixels.size(), f);
        std::fclose(f);
    }
    return true;
}

bool write_png(const char* path, u32 width, u32 height, const u8* rgba) {
    return stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 4, rgba, static_cast<int>(width * 4)) != 0;
}

bool texture_size(const char* path, u32& width, u32& height) {
    int w = 0, h = 0, channels = 0;
    if (bool exr; is_float_image(path, exr) && exr) {  // the header only
        EXRVersion version;
        EXRHeader header;
        InitEXRHeader(&header);
        const char* err = nullptr;
        const bool ok = ParseEXRVersionFromFile(&version, path) == TINYEXR_SUCCESS && ParseEXRHeaderFromFile(&header, &version, path, &err) == TINYEXR_SUCCESS;
        if (ok) {
            width = static_cast<u32>(header.data_window.max_x - header.data_window.min_x + 1);
            height = static_cast<u32>(header.data_window.max_y - header.data_window.min_y + 1);
        }
        FreeEXRErrorMessage(err);
        FreeEXRHeader(&header);
        return ok;
    }
    if (!stbi_info(path, &w, &h, &channels)) return false;
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
    return true;
}

} // namespace lsd::asset
