#include "texture_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace plumnin {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'P', 'L', 'C', '1'};
constexpr size_t kHeader = sizeof(kMagic) + 8;  // the magic and the key
constexpr const char* kExtension = ".qoi";

uint64_t fnv(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// QOI's operations.
constexpr uint8_t kIndex = 0x00, kDiff = 0x40, kLuma = 0x80, kRun = 0xC0, kRgb = 0xFE, kRgba = 0xFF;
constexpr uint8_t kEnd[8] = {0, 0, 0, 0, 0, 0, 0, 1};

inline int hashOf(uint32_t px) {
    const uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF, a = px >> 24;
    return static_cast<int>((r * 3 + g * 5 + b * 7 + a * 11) % 64);
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

}  // namespace

std::vector<uint8_t> TextureCache::encodeQoi(const std::vector<uint32_t>& pixels, int width, int height) {
    std::vector<uint8_t> out;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (width <= 0 || height <= 0 || pixels.size() < count) return out;
    out.reserve(14 + count * 2);
    out.insert(out.end(), {'q', 'o', 'i', 'f'});
    put32(out, static_cast<uint32_t>(width));
    put32(out, static_cast<uint32_t>(height));
    out.push_back(4);  // RGBA
    out.push_back(0);  // sRGB with linear alpha
    uint32_t index[64] = {};
    uint32_t prev = 0xFF000000u;
    int run = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t px = pixels[i];
        if (px == prev) {
            if (++run == 62 || i + 1 == count) {
                out.push_back(static_cast<uint8_t>(kRun | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run > 0) {
            out.push_back(static_cast<uint8_t>(kRun | (run - 1)));
            run = 0;
        }
        const int h = hashOf(px);
        if (index[h] == px) {
            out.push_back(static_cast<uint8_t>(kIndex | h));
        } else {
            index[h] = px;
            if ((px >> 24) == (prev >> 24)) {
                const int vr = static_cast<int8_t>(static_cast<uint8_t>((px & 0xFF) - (prev & 0xFF)));
                const int vg = static_cast<int8_t>(static_cast<uint8_t>(((px >> 8) & 0xFF) - ((prev >> 8) & 0xFF)));
                const int vb = static_cast<int8_t>(static_cast<uint8_t>(((px >> 16) & 0xFF) - ((prev >> 16) & 0xFF)));
                const int vgr = vr - vg, vgb = vb - vg;
                if (vr >= -2 && vr <= 1 && vg >= -2 && vg <= 1 && vb >= -2 && vb <= 1) {
                    out.push_back(static_cast<uint8_t>(kDiff | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2)));
                } else if (vg >= -32 && vg <= 31 && vgr >= -8 && vgr <= 7 && vgb >= -8 && vgb <= 7) {
                    out.push_back(static_cast<uint8_t>(kLuma | (vg + 32)));
                    out.push_back(static_cast<uint8_t>((vgr + 8) << 4 | (vgb + 8)));
                } else {
                    out.push_back(kRgb);
                    out.push_back(static_cast<uint8_t>(px));
                    out.push_back(static_cast<uint8_t>(px >> 8));
                    out.push_back(static_cast<uint8_t>(px >> 16));
                }
            } else {
                out.push_back(kRgba);
                for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(px >> s));
            }
        }
        prev = px;
    }
    out.insert(out.end(), std::begin(kEnd), std::end(kEnd));
    return out;
}

bool TextureCache::decodeQoi(const uint8_t* data, size_t size, std::vector<uint32_t>& pixels, int* width, int* height) {
    if (size < 14 + sizeof(kEnd) || std::memcmp(data, "qoif", 4) != 0) return false;
    const uint32_t w = get32(data + 4), h = get32(data + 8);
    // Covers are a few thousand pixels at most; anything bigger is not one of ours.
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    const size_t count = static_cast<size_t>(w) * h;
    pixels.resize(count);
    uint32_t index[64] = {};
    uint32_t px = 0xFF000000u;
    size_t p = 14;
    const size_t end = size - sizeof(kEnd);
    int run = 0;
    for (size_t i = 0; i < count; ++i) {
        if (run > 0) {
            --run;
        } else {
            if (p >= end) return false;
            const uint8_t b1 = data[p++];
            if (b1 == kRgb) {
                if (p + 3 > end) return false;
                px = (px & 0xFF000000u) | data[p] | static_cast<uint32_t>(data[p + 1]) << 8 | static_cast<uint32_t>(data[p + 2]) << 16;
                p += 3;
            } else if (b1 == kRgba) {
                if (p + 4 > end) return false;
                px = data[p] | static_cast<uint32_t>(data[p + 1]) << 8 | static_cast<uint32_t>(data[p + 2]) << 16 |
                     static_cast<uint32_t>(data[p + 3]) << 24;
                p += 4;
            } else if ((b1 & 0xC0) == kIndex) {
                px = index[b1];
            } else if ((b1 & 0xC0) == kDiff) {
                const uint32_t r = ((px & 0xFF) + ((b1 >> 4) & 3) - 2) & 0xFF;
                const uint32_t g = (((px >> 8) & 0xFF) + ((b1 >> 2) & 3) - 2) & 0xFF;
                const uint32_t b = (((px >> 16) & 0xFF) + (b1 & 3) - 2) & 0xFF;
                px = (px & 0xFF000000u) | r | g << 8 | b << 16;
            } else if ((b1 & 0xC0) == kLuma) {
                if (p + 1 > end) return false;
                const uint8_t b2 = data[p++];
                const int vg = (b1 & 0x3F) - 32;
                const uint32_t r = ((px & 0xFF) + vg - 8 + ((b2 >> 4) & 0x0F)) & 0xFF;
                const uint32_t g = (((px >> 8) & 0xFF) + vg) & 0xFF;
                const uint32_t b = (((px >> 16) & 0xFF) + vg - 8 + (b2 & 0x0F)) & 0xFF;
                px = (px & 0xFF000000u) | r | g << 8 | b << 16;
            } else {
                run = b1 & 0x3F;
            }
            index[hashOf(px)] = px;
        }
        pixels[i] = px;
    }
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
}

uint64_t TextureCache::key(const std::string& path, uint32_t variant) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) return 0;
    const auto time = fs::last_write_time(p, ec);
    if (ec) return 0;
    const int64_t ticks = static_cast<int64_t>(time.time_since_epoch().count());
    const uint64_t bytes = static_cast<uint64_t>(size);
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, path.data(), path.size());
    h = fnv(h, &bytes, sizeof(bytes));
    h = fnv(h, &ticks, sizeof(ticks));
    h = fnv(h, &variant, sizeof(variant));
    return h ? h : 1;
}

std::string TextureCache::file(uint64_t key) const {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
    return folder_ + name + kExtension;
}

bool TextureCache::load(uint64_t key, std::vector<uint32_t>& pixels, int* width, int* height) const {
    if (folder_.empty() || !key) return false;
    std::ifstream in(fs::u8path(file(key)), std::ios::binary);
    if (!in) return false;
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (data.size() < kHeader || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) return false;
    uint64_t stored = 0;
    std::memcpy(&stored, data.data() + sizeof(kMagic), sizeof(stored));
    if (stored != key) return false;
    return decodeQoi(data.data() + kHeader, data.size() - kHeader, pixels, width, height);
}

void TextureCache::store(uint64_t key, const std::vector<uint32_t>& pixels, int width, int height) const {
    if (folder_.empty() || !key) return;
    const std::vector<uint8_t> qoi = encodeQoi(pixels, width, height);
    if (qoi.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(folder_), ec);
    const std::string target = file(key);
    const fs::path temporary = fs::u8path(target + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(kMagic, sizeof(kMagic));
        out.write(reinterpret_cast<const char*>(&key), sizeof(key));
        out.write(reinterpret_cast<const char*>(qoi.data()), static_cast<std::streamsize>(qoi.size()));
        if (!out) {
            out.close();
            fs::remove(temporary, ec);
            return;
        }
    }
    fs::rename(temporary, fs::u8path(target), ec);
    if (ec) fs::remove(temporary, ec);
}

void TextureCache::prune(const std::vector<uint64_t>& keep) const {
    if (folder_.empty()) return;
    std::error_code ec;
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(fs::u8path(folder_), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        const bool temporary = name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
        if (!temporary && it->path().extension().u8string() != kExtension) continue;
        if (!temporary) {
            char* rest = nullptr;
            const unsigned long long k = std::strtoull(it->path().stem().u8string().c_str(), &rest, 16);
            if (std::binary_search(keep.begin(), keep.end(), static_cast<uint64_t>(k))) continue;
        }
        stale.push_back(it->path());
    }
    for (const fs::path& p : stale) fs::remove(p, ec);
}

}  // namespace plumnin
