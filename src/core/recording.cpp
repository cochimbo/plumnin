#include "recording.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "i18n.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace plumnin {

namespace {

constexpr int kJpegQuality = 85;
constexpr double kGifMaxFps = 30;  // GIF delays are in hundredths of a second, and viewers slow down faster ones
// An AVI's parts (RIFF chunks) stay under 1 GB, which every player reads.
constexpr uint64_t kAviPartBytes = uint64_t{1000} << 20;
constexpr int kAviMaxParts = 256;  // room in the header for 256 parts: 250 GB

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}
void putTag(std::vector<uint8_t>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

// --- GIF ---

// Up to 255 colours for a frame (the 256th is the transparent one) by median cut over
// its colours at 5 bits per channel. A Game Boy or NES picture has fewer than that, and
// each comes out exact: a palette entry is the average of the real colours behind it.
void quantize(const std::vector<uint32_t>& pixels, std::vector<uint32_t>& palette, std::vector<uint8_t>& index) {
    std::vector<uint32_t> count(32768);
    std::vector<uint64_t> sumR(32768), sumG(32768), sumB(32768);
    std::vector<uint16_t> bins;
    auto binOf = [](uint32_t p) { return static_cast<uint16_t>((p >> 9 & 0x7C00) | (p >> 6 & 0x3E0) | (p >> 3 & 0x1F)); };
    for (uint32_t p : pixels) {
        const uint16_t b = binOf(p);
        if (count[b]++ == 0) bins.push_back(b);
        sumR[b] += p >> 16 & 0xFF;
        sumG[b] += p >> 8 & 0xFF;
        sumB[b] += p & 0xFF;
    }
    struct Box {
        size_t begin, end;
        uint64_t count;
        int axis, range;
    };
    auto channel = [](uint16_t b, int axis) { return b >> (10 - 5 * axis) & 31; };
    auto measure = [&](Box& box) {
        int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
        box.count = 0;
        for (size_t i = box.begin; i < box.end; ++i) {
            box.count += count[bins[i]];
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], channel(bins[i], a));
                hi[a] = std::max(hi[a], channel(bins[i], a));
            }
        }
        box.axis = 0;
        for (int a = 1; a < 3; ++a)
            if (hi[a] - lo[a] > hi[box.axis] - lo[box.axis]) box.axis = a;
        box.range = hi[box.axis] - lo[box.axis];
    };
    std::vector<Box> boxes;
    if (bins.size() <= 255) {
        for (size_t i = 0; i < bins.size(); ++i) boxes.push_back({i, i + 1, count[bins[i]], 0, 0});
    } else {
        boxes.push_back({0, bins.size(), 0, 0, 0});
        measure(boxes[0]);
        while (boxes.size() < 255) {
            // The box with the most pixels over the widest span of colour goes in two.
            size_t pick = boxes.size();
            double best = 0;
            for (size_t i = 0; i < boxes.size(); ++i) {
                const double score = static_cast<double>(boxes[i].count) * boxes[i].range;
                if (boxes[i].end - boxes[i].begin > 1 && score > best) {
                    best = score;
                    pick = i;
                }
            }
            if (pick == boxes.size()) break;
            Box& box = boxes[pick];
            const int axis = box.axis;
            std::sort(bins.begin() + static_cast<ptrdiff_t>(box.begin), bins.begin() + static_cast<ptrdiff_t>(box.end),
                      [&](uint16_t a, uint16_t b) { return channel(a, axis) < channel(b, axis); });
            size_t mid = box.begin + 1;
            for (uint64_t seen = count[bins[box.begin]]; mid + 1 < box.end && seen * 2 < box.count; ++mid)
                seen += count[bins[mid]];
            Box upper{mid, box.end, 0, 0, 0};
            box.end = mid;
            measure(box);
            measure(upper);
            boxes.push_back(upper);
        }
    }
    std::vector<uint8_t> map(32768);
    palette.clear();
    for (const Box& box : boxes) {
        uint64_t r = 0, g = 0, b = 0, n = 0;
        for (size_t i = box.begin; i < box.end; ++i) {
            const uint16_t bin = bins[i];
            r += sumR[bin];
            g += sumG[bin];
            b += sumB[bin];
            n += count[bin];
            map[bin] = static_cast<uint8_t>(palette.size());
        }
        palette.push_back(static_cast<uint32_t>((r + n / 2) / n << 16 | (g + n / 2) / n << 8 | (b + n / 2) / n));
    }
    index.resize(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i) index[i] = map[binOf(pixels[i])];
}

// GIF's LZW compression of colour indices of minCode bits, in 255-byte blocks.
void lzw(const std::vector<uint8_t>& data, int minCode, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(minCode));
    std::vector<uint8_t> block;
    uint32_t bits = 0;
    int bitCount = 0;
    auto flushBlock = [&] {
        if (block.empty()) return;
        out.push_back(static_cast<uint8_t>(block.size()));
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
    };
    auto emit = [&](int code, int size) {
        bits |= static_cast<uint32_t>(code) << bitCount;
        bitCount += size;
        for (; bitCount >= 8; bitCount -= 8, bits >>= 8) {
            block.push_back(static_cast<uint8_t>(bits));
            if (block.size() == 255) flushBlock();
        }
    };
    const int clear = 1 << minCode, end = clear + 1;
    // The strings seen so far: (prefix code << 8 | next index) -> code, open addressing.
    constexpr int kTable = 8192;
    std::vector<int32_t> keys(kTable, -1);
    std::vector<int16_t> codes(kTable);
    int codeSize = minCode + 1, lastCode = end;
    emit(clear, codeSize);
    int current = data.empty() ? 0 : data[0];
    for (size_t i = 1; i < data.size(); ++i) {
        const int32_t key = current << 8 | data[i];
        size_t slot = static_cast<size_t>(key * 2654435761u >> 19) & (kTable - 1);
        while (keys[slot] >= 0 && keys[slot] != key) slot = (slot + 1) & (kTable - 1);
        if (keys[slot] == key) {
            current = codes[slot];
            continue;
        }
        emit(current, codeSize);
        keys[slot] = key;
        codes[slot] = static_cast<int16_t>(++lastCode);
        if (lastCode >= (1 << codeSize) && codeSize < 12) ++codeSize;
        if (lastCode == 4095) {  // the table is full: start over
            emit(clear, codeSize);
            std::fill(keys.begin(), keys.end(), -1);
            codeSize = minCode + 1;
            lastCode = end;
        }
        current = data[i];
    }
    emit(current, codeSize);
    // The decoder adds a code for that last one too, and may need a bit more for the end.
    if (lastCode + 1 >= (1 << codeSize) && codeSize < 12) ++codeSize;
    emit(end, codeSize);
    if (bitCount > 0) block.push_back(static_cast<uint8_t>(bits));
    flushBlock();
    out.push_back(0);
}

// Writes the frames one after another. Each frame has its own palette and only covers
// the rectangle that changed, with what stayed the same transparent; a frame where
// nothing changed only lengthens the one before.
class GifEncoder {
public:
    void begin(int width, int height, std::vector<uint8_t>& out) {
        width_ = width;
        height_ = height;
        shown_.assign(static_cast<size_t>(width) * height, 0);
        out.insert(out.end(), {'G', 'I', 'F', '8', '9', 'a'});
        put16(out, static_cast<uint32_t>(width));
        put16(out, static_cast<uint32_t>(height));
        out.insert(out.end(), {0, 0, 0});
        // Loops forever (the NETSCAPE2.0 extension).
        out.insert(out.end(), {0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0});
    }

    // A frame shown for `delay` hundredths of a second, as quantize() left it.
    void frame(const std::vector<uint32_t>& palette, const std::vector<uint8_t>& index, int delay, std::vector<uint8_t>& out) {
        const auto transparent = static_cast<uint8_t>(palette.size());
        int left = 0, top = 0, right = width_ - 1, bottom = height_ - 1;
        if (frames_ > 0) {
            left = width_;
            top = height_;
            right = bottom = -1;
            for (int y = 0; y < height_; ++y)
                for (int x = 0; x < width_; ++x) {
                    const size_t i = static_cast<size_t>(y) * width_ + x;
                    if (palette[index[i]] == shown_[i]) continue;
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            if (right < 0) {
                pendingDelay_ += delay;
                return;
            }
        }
        flush(out);
        int bits = 2;
        while ((1 << bits) < static_cast<int>(palette.size()) + 1) ++bits;
        const int w = right - left + 1, h = bottom - top + 1;
        std::vector<uint8_t> data(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(top + y) * width_ + left + x;
                const uint32_t colour = palette[index[i]];
                if (frames_ > 0 && colour == shown_[i]) {
                    data[static_cast<size_t>(y) * w + x] = transparent;
                } else {
                    data[static_cast<size_t>(y) * w + x] = index[i];
                    shown_[i] = colour;
                }
            }
        pending_.clear();
        pending_.push_back(0x2C);
        put16(pending_, static_cast<uint32_t>(left));
        put16(pending_, static_cast<uint32_t>(top));
        put16(pending_, static_cast<uint32_t>(w));
        put16(pending_, static_cast<uint32_t>(h));
        pending_.push_back(static_cast<uint8_t>(0x80 | (bits - 1)));  // a palette of its own
        for (int i = 0; i < (1 << bits); ++i) {
            const uint32_t c = i < static_cast<int>(palette.size()) ? palette[static_cast<size_t>(i)] : 0;
            pending_.insert(pending_.end(), {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)});
        }
        lzw(data, bits, pending_);
        pendingTransparent_ = frames_ > 0 ? transparent : -1;
        pendingDelay_ = delay;
        ++frames_;
    }

    void end(std::vector<uint8_t>& out) {
        flush(out);
        out.push_back(0x3B);
    }

private:
    // The previous frame, now that its delay is known.
    void flush(std::vector<uint8_t>& out) {
        if (pending_.empty()) return;
        const int delay = std::min(pendingDelay_, 65535);
        out.insert(out.end(), {0x21, 0xF9, 4});
        out.push_back(static_cast<uint8_t>(1 << 2 | (pendingTransparent_ >= 0 ? 1 : 0)));  // keep it under the next
        put16(out, static_cast<uint32_t>(delay));
        out.push_back(static_cast<uint8_t>(std::max(pendingTransparent_, 0)));
        out.push_back(0);
        out.insert(out.end(), pending_.begin(), pending_.end());
        pending_.clear();
    }

    int width_ = 0, height_ = 0;
    int frames_ = 0;
    std::vector<uint32_t> shown_;  // what a viewer sees after the frames so far
    std::vector<uint8_t> pending_;
    int pendingDelay_ = 0;
    int pendingTransparent_ = -1;
};

// --- AVI ---

// An OpenDML AVI: a first RIFF 'AVI ' part with the headers and the old idx1 index,
// then RIFF 'AVIX' parts, each part with its own index of each stream, and in the
// header a list of those indexes. Video is stream 0 (00dc), sound stream 1 (01wb).
class AviWriter {
public:
    void begin(int width, int height, double fps, int sampleRate) {
        width_ = width;
        height_ = height;
        rate_ = static_cast<uint32_t>(std::llround(fps * 1000000.0));
        sampleRate_ = static_cast<uint32_t>(sampleRate);
    }

    // The headers as they stand: written at the start, and again at the end.
    std::vector<uint8_t> header() const {
        std::vector<uint8_t> v;
        const uint32_t indexSize = 24 + 16 * kAviMaxParts;
        const uint32_t strlVideo = 4 + 8 + 56 + 8 + 40 + 8 + indexSize;
        const uint32_t strlAudio = 4 + 8 + 56 + 8 + 18 + 8 + indexSize;
        const uint32_t odml = 4 + 8 + 248;
        const uint32_t hdrl = 4 + 8 + 56 + 8 + strlVideo + 8 + strlAudio + 8 + odml;
        putTag(v, "RIFF");
        put32(v, 0);  // the first part's size, filled in by finishPart()
        putTag(v, "AVI ");
        putTag(v, "LIST");
        put32(v, hdrl);
        putTag(v, "hdrl");
        putTag(v, "avih");
        put32(v, 56);
        put32(v, static_cast<uint32_t>(1e12 / rate_));  // microseconds a frame
        put32(v, 0);
        put32(v, 0);
        put32(v, 0x10 | 0x100);  // has an index, interleaved
        put32(v, parts_.empty() ? 0 : parts_[0].frames);
        put32(v, 0);
        put32(v, 2);
        put32(v, largestChunk_);
        put32(v, static_cast<uint32_t>(width_));
        put32(v, static_cast<uint32_t>(height_));
        for (int i = 0; i < 4; ++i) put32(v, 0);
        // The picture.
        putTag(v, "LIST");
        put32(v, strlVideo);
        putTag(v, "strl");
        streamHeader(v, "vids", "MJPG", 1000000, rate_, frames_, 0);
        putTag(v, "strf");
        put32(v, 40);
        put32(v, 40);
        put32(v, static_cast<uint32_t>(width_));
        put32(v, static_cast<uint32_t>(height_));
        put16(v, 1);
        put16(v, 24);
        putTag(v, "MJPG");
        put32(v, static_cast<uint32_t>(width_ * height_ * 3));
        for (int i = 0; i < 4; ++i) put32(v, 0);
        superIndex(v, "00dc", 0);
        // The sound.
        putTag(v, "LIST");
        put32(v, strlAudio);
        putTag(v, "strl");
        streamHeader(v, "auds", "\0\0\0\0", 4, sampleRate_ * 4, samples_, 4);
        putTag(v, "strf");
        put32(v, 18);
        put16(v, 1);  // PCM
        put16(v, 2);
        put32(v, sampleRate_);
        put32(v, sampleRate_ * 4);
        put16(v, 4);
        put16(v, 16);
        put16(v, 0);
        superIndex(v, "01wb", 1);
        putTag(v, "LIST");
        put32(v, odml);
        putTag(v, "odml");
        putTag(v, "dmlh");
        put32(v, 248);
        put32(v, frames_);
        v.resize(v.size() + 244);
        return v;
    }

    // Writes a frame and its sound; a new part starts when this one would pass 1 GB.
    bool frame(std::ofstream& f, const std::vector<uint8_t>& jpeg, const std::vector<int16_t>& audio) {
        const uint64_t bytes = 8 + jpeg.size() + 1 + 8 + audio.size() * 2 + 1;
        const uint64_t indexes = (part().chunks[0].size() + part().chunks[1].size() + 2) * (parts_.size() == 1 ? 24 : 8) + 64;
        if (static_cast<uint64_t>(f.tellp()) - part().start + bytes + indexes > kAviPartBytes) {
            if (parts_.size() == static_cast<size_t>(kAviMaxParts)) return false;
            finishPart(f);
            startPart(f);
        }
        chunk(f, 0, jpeg.data(), static_cast<uint32_t>(jpeg.size()));
        ++frames_;
        ++part().frames;
        if (!audio.empty()) {
            chunk(f, 1, audio.data(), static_cast<uint32_t>(audio.size() * 2));
            samples_ += static_cast<uint32_t>(audio.size() / 2);
            part().samples += static_cast<uint32_t>(audio.size() / 2);
        }
        return static_cast<bool>(f);
    }

    void startPart(std::ofstream& f) {
        Part p;
        p.start = static_cast<uint64_t>(f.tellp());
        std::vector<uint8_t> v;
        if (!parts_.empty()) {
            putTag(v, "RIFF");
            put32(v, 0);
            putTag(v, "AVIX");
        }
        p.movi = p.start + v.size();
        putTag(v, "LIST");
        put32(v, 0);
        putTag(v, "movi");
        write(f, v);
        parts_.push_back(p);
    }

    // Closes the part: its indexes go at the end of its movi list, and its sizes in place.
    void finishPart(std::ofstream& f) {
        Part& p = part();
        static const char* const kIds[2] = {"00dc", "01wb"};
        static const char* const kIndexIds[2] = {"ix00", "ix01"};
        for (int s = 0; s < 2; ++s) {
            p.index[s] = static_cast<uint64_t>(f.tellp());
            std::vector<uint8_t> v;
            putTag(v, kIndexIds[s]);
            put32(v, 24 + 8 * static_cast<uint32_t>(p.chunks[s].size()));
            put16(v, 2);
            v.push_back(0);
            v.push_back(1);  // an index of chunks
            put32(v, static_cast<uint32_t>(p.chunks[s].size()));
            putTag(v, kIds[s]);
            put64(v, p.start);
            put32(v, 0);
            for (const Chunk& c : p.chunks[s]) {
                put32(v, static_cast<uint32_t>(c.offset + 8 - p.start));
                put32(v, c.size);
            }
            p.indexSize[s] = static_cast<uint32_t>(v.size());
            write(f, v);
        }
        const uint64_t moviEnd = static_cast<uint64_t>(f.tellp());
        if (parts_.size() == 1) {
            // The old index, for players that know nothing else.
            std::vector<uint8_t> v;
            putTag(v, "idx1");
            put32(v, static_cast<uint32_t>(16 * order_.size()));
            for (const auto& [stream, i] : order_) {
                const Chunk& c = p.chunks[stream][i];
                putTag(v, kIds[stream]);
                put32(v, 0x10);  // a key frame
                put32(v, static_cast<uint32_t>(c.offset - (p.movi + 8)));
                put32(v, c.size);
            }
            write(f, v);
            order_.clear();
        }
        const uint64_t end = static_cast<uint64_t>(f.tellp());
        patch32(f, p.movi + 4, static_cast<uint32_t>(moviEnd - p.movi - 8));
        if (parts_.size() > 1) patch32(f, p.start + 4, static_cast<uint32_t>(end - p.start - 8));
        else riffSize_ = static_cast<uint32_t>(end - 8);
        f.seekp(static_cast<std::streamoff>(end));
    }

    // The first part's RIFF size, once finished.
    uint32_t riffSize() const { return riffSize_; }

private:
    struct Chunk {
        uint64_t offset;  // of its header in the file
        uint32_t size;
    };
    struct Part {
        uint64_t start = 0, movi = 0;
        std::vector<Chunk> chunks[2];
        uint32_t frames = 0, samples = 0;
        uint64_t index[2] = {0, 0};
        uint32_t indexSize[2] = {0, 0};
    };

    Part& part() { return parts_.back(); }

    static void write(std::ofstream& f, const std::vector<uint8_t>& v) {
        f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
    }

    static void patch32(std::ofstream& f, uint64_t at, uint32_t value) {
        std::vector<uint8_t> v;
        put32(v, value);
        f.seekp(static_cast<std::streamoff>(at));
        write(f, v);
    }

    void chunk(std::ofstream& f, int stream, const void* data, uint32_t size) {
        Part& p = part();
        p.chunks[stream].push_back({static_cast<uint64_t>(f.tellp()), size});
        if (parts_.size() == 1) order_.emplace_back(stream, p.chunks[stream].size() - 1);
        std::vector<uint8_t> v;
        putTag(v, stream == 0 ? "00dc" : "01wb");
        put32(v, size);
        write(f, v);
        f.write(static_cast<const char*>(data), size);
        if (size & 1) f.put(0);
        largestChunk_ = std::max(largestChunk_, size);
    }

    void streamHeader(std::vector<uint8_t>& v, const char* type, const char* handler, uint32_t scale, uint32_t rate,
                      uint32_t length, uint32_t sampleSize) const {
        putTag(v, "strh");
        put32(v, 56);
        putTag(v, type);
        putTag(v, handler);
        put32(v, 0);
        put16(v, 0);
        put16(v, 0);
        put32(v, 0);
        put32(v, scale);
        put32(v, rate);
        put32(v, 0);
        put32(v, length);
        put32(v, largestChunk_);
        put32(v, 0xFFFFFFFF);
        put32(v, sampleSize);
        put16(v, 0);
        put16(v, 0);
        put16(v, sampleSize ? 0 : static_cast<uint32_t>(width_));
        put16(v, sampleSize ? 0 : static_cast<uint32_t>(height_));
    }

    // The list of a stream's indexes, one per finished part.
    void superIndex(std::vector<uint8_t>& v, const char* id, int stream) const {
        putTag(v, "indx");
        put32(v, 24 + 16 * kAviMaxParts);
        put16(v, 4);
        v.push_back(0);
        v.push_back(0);  // an index of indexes
        uint32_t used = 0;
        for (const Part& p : parts_) used += p.index[stream] ? 1 : 0;
        put32(v, used);
        putTag(v, id);
        put32(v, 0);
        put32(v, 0);
        put32(v, 0);
        for (int i = 0; i < kAviMaxParts; ++i) {
            const bool has = i < static_cast<int>(parts_.size()) && parts_[static_cast<size_t>(i)].index[stream];
            const Part* p = has ? &parts_[static_cast<size_t>(i)] : nullptr;
            put64(v, p ? p->index[stream] : 0);
            put32(v, p ? p->indexSize[stream] : 0);
            put32(v, p ? (stream == 0 ? p->frames : p->samples) : 0);
        }
    }

    int width_ = 0, height_ = 0;
    uint32_t rate_ = 60000000;  // frames a second, in millionths
    uint32_t sampleRate_ = 44100;
    uint32_t frames_ = 0, samples_ = 0;
    uint32_t largestChunk_ = 0;
    uint32_t riffSize_ = 0;
    std::vector<Part> parts_;
    std::vector<std::pair<int, size_t>> order_;  // the first part's chunks, in file order (for idx1)
};

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

}  // namespace

void scalePicture(const uint32_t* src, int srcWidth, int srcHeight, ptrdiff_t stride, uint32_t* dst, int dstWidth,
                  int dstHeight) {
    std::vector<int> xs(static_cast<size_t>(dstWidth) + 1);
    for (int x = 0; x <= dstWidth; ++x) xs[static_cast<size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * srcWidth / dstWidth);
    for (int y = 0; y < dstHeight; ++y) {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * srcHeight / dstHeight);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * srcHeight / dstHeight));
        uint32_t* out = dst + static_cast<ptrdiff_t>(y) * dstWidth;
        for (int x = 0; x < dstWidth; ++x) {
            const int x0 = xs[static_cast<size_t>(x)];
            const int x1 = std::max(x0 + 1, xs[static_cast<size_t>(x) + 1]);
            if (x1 - x0 == 1 && y1 - y0 == 1) {
                out[x] = src[static_cast<ptrdiff_t>(y0) * stride + x0] | 0xFF000000u;
                continue;
            }
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint32_t* row = src + static_cast<ptrdiff_t>(sy) * stride;
                for (int sx = x0; sx < x1; ++sx) {
                    r += row[sx] >> 16 & 0xFF;
                    g += row[sx] >> 8 & 0xFF;
                    b += row[sx] & 0xFF;
                }
            }
            const uint32_t n = static_cast<uint32_t>((x1 - x0) * (y1 - y0));
            out[x] = 0xFF000000u | (r + n / 2) / n << 16 | (g + n / 2) / n << 8 | (b + n / 2) / n;
        }
    }
}

struct GameRecording::Impl {
    struct Job {
        std::vector<uint32_t> pixels;
        std::vector<int16_t> audio;
        int delay = 0;  // GIF: hundredths of a second it is shown
        // What the encoding threads make of it: a JPEG, or a GIF palette and indices.
        std::vector<uint8_t> encoded;
        std::vector<uint32_t> palette;
        bool taken = false, done = false;
    };

    Format format = Format::Gif;
    std::string path;
    int width = 0, height = 0;
    double fps = 60;
    int sampleRate = 44100;
    bool active = false;

    // Only the game's thread touches these.
    uint64_t framesIn = 0;    // frames given to addFrame()
    uint64_t framesKept = 0;  // of them, the ones recorded (a GIF skips some)
    int gifStep = 1;          // a GIF keeps one frame in gifStep
    uint64_t audioOut = 0;    // stereo samples handed to the jobs
    std::vector<int16_t> audio;

    std::ofstream file;
    GifEncoder gif;
    AviWriter avi;

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<Job>> jobs;
    size_t maxJobs = 8;
    bool writing = false, stopping = false, failed = false;
    std::string error;
    std::vector<std::thread> threads;

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed) error = why;
        failed = true;
    }

    void encode(Job& job) {
        if (format == Format::Gif) {
            std::vector<uint8_t> index;
            quantize(job.pixels, job.palette, index);
            job.encoded = std::move(index);
            return;
        }
        std::vector<uint8_t> rgb(job.pixels.size() * 3);
        for (size_t i = 0; i < job.pixels.size(); ++i) {
            rgb[i * 3] = static_cast<uint8_t>(job.pixels[i] >> 16);
            rgb[i * 3 + 1] = static_cast<uint8_t>(job.pixels[i] >> 8);
            rgb[i * 3 + 2] = static_cast<uint8_t>(job.pixels[i]);
        }
        stbi_write_jpg_to_func(appendBytes, &job.encoded, width, height, 3, rgb.data(), kJpegQuality);
    }

    // In order, one at a time.
    void write(Job& job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failed) return;
        }
        bool ok = true;
        if (format == Format::Gif) {
            std::vector<uint8_t> out;
            gif.frame(job.palette, job.encoded, job.delay, out);
            file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            ok = static_cast<bool>(file);
        } else {
            ok = !job.encoded.empty() && avi.frame(file, job.encoded, job.audio);
        }
        if (!ok) fail(trf("No se pudo escribir %s: ¿falta espacio en el disco?", path.c_str()));
    }

    void work() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (!writing && !jobs.empty() && jobs.front()->done) {
                std::unique_ptr<Job> job = std::move(jobs.front());
                jobs.pop_front();
                writing = true;
                changed.notify_all();
                lock.unlock();
                write(*job);
                lock.lock();
                writing = false;
                changed.notify_all();
                continue;
            }
            Job* next = nullptr;
            for (auto& j : jobs)
                if (!j->taken) {
                    next = j.get();
                    break;
                }
            if (next) {
                next->taken = true;
                lock.unlock();
                encode(*next);
                lock.lock();
                next->done = true;
                changed.notify_all();
                continue;
            }
            if (stopping && jobs.empty()) return;
            changed.wait(lock);
        }
    }
};

GameRecording::GameRecording() : impl_(std::make_unique<Impl>()) {}

GameRecording::~GameRecording() { finish(); }

bool GameRecording::start(const std::string& path, Format format, int width, int height, double fps, int sampleRate,
                          std::string* error) {
    finish();
    impl_ = std::make_unique<Impl>();
    Impl& r = *impl_;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || fps <= 0 || sampleRate <= 0) {
        if (error) *error = tr("No se puede grabar con ese tamaño");
        return false;
    }
    r.format = format;
    r.path = path;
    r.width = width;
    r.height = height;
    r.fps = fps;
    r.sampleRate = sampleRate;
    r.gifStep = std::max(1, static_cast<int>(std::ceil(fps / kGifMaxFps - 0.01)));
    std::error_code ec;
    const auto fsPath = std::filesystem::u8path(path);
    if (fsPath.has_parent_path()) std::filesystem::create_directories(fsPath.parent_path(), ec);
    r.file.open(fsPath, std::ios::binary | std::ios::trunc);
    if (!r.file) {
        if (error) *error = trf("No se pudo crear %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> head;
    if (format == Format::Gif) {
        r.gif.begin(width, height, head);
        r.file.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    } else {
        r.avi.begin(width, height, fps, sampleRate);
        head = r.avi.header();
        r.file.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        r.avi.startPart(r.file);
    }
    // JPEGs are encoded several at a time; a GIF needs fewer hands.
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const unsigned threads = format == Format::Avi ? std::min(4u, cores - 1) : std::min(2u, cores - 1);
    r.maxJobs = 2 + threads * 2;
    r.active = true;
    for (unsigned i = 0; i < threads; ++i) r.threads.emplace_back([&r] { r.work(); });
    return true;
}

bool GameRecording::active() const { return impl_->active; }
GameRecording::Format GameRecording::format() const { return impl_->format; }
const std::string& GameRecording::path() const { return impl_->path; }

void GameRecording::addAudio(const int16_t* samples, size_t count) {
    Impl& r = *impl_;
    if (!r.active || r.format != Format::Avi) return;
    r.audio.insert(r.audio.end(), samples, samples + (count & ~size_t{1}));
}

void GameRecording::addFrame(const uint32_t* pixels, int width, int height, ptrdiff_t stride) {
    Impl& r = *impl_;
    if (!r.active || width <= 0 || height <= 0) return;
    const uint64_t n = r.framesIn++;
    auto job = std::make_unique<Impl::Job>();
    if (r.format == Format::Gif) {
        if (n % static_cast<uint64_t>(r.gifStep) != 0) return;
        const uint64_t k = r.framesKept;
        auto hundredths = [&](uint64_t frame) { return std::llround(static_cast<double>(frame * r.gifStep) * 100.0 / r.fps); };
        job->delay = static_cast<int>(std::max<int64_t>(2, hundredths(k + 1) - hundredths(k)));
    } else {
        // The sound stays with the picture: what is missing (a pause, a menu) is
        // silence, and what is too much is dropped.
        const double perFrame = r.sampleRate / r.fps;
        const auto target = static_cast<int64_t>(std::llround(static_cast<double>(r.framesKept + 1) * perFrame));
        const auto have = static_cast<int64_t>(r.audioOut + r.audio.size() / 2);
        if (have < target - perFrame) r.audio.resize(r.audio.size() + static_cast<size_t>(target - have) * 2, 0);
        else if (have > target + perFrame)
            r.audio.resize(r.audio.size() - static_cast<size_t>(std::min<int64_t>(have - target, static_cast<int64_t>(r.audio.size() / 2))) * 2);
        r.audioOut += r.audio.size() / 2;
        job->audio = std::move(r.audio);
        r.audio.clear();
    }
    ++r.framesKept;
    job->pixels.resize(static_cast<size_t>(r.width) * r.height);
    scalePicture(pixels, width, height, stride, job->pixels.data(), r.width, r.height);
    std::unique_lock<std::mutex> lock(r.mutex);
    if (r.failed) return;
    r.changed.wait(lock, [&] { return r.jobs.size() < r.maxJobs; });
    r.jobs.push_back(std::move(job));
    r.changed.notify_all();
}

double GameRecording::seconds() const { return impl_->active ? static_cast<double>(impl_->framesIn) / impl_->fps : 0.0; }

bool GameRecording::failed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failed;
}

std::string GameRecording::error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

bool GameRecording::finish(std::string* error) {
    Impl& r = *impl_;
    if (!r.active) return true;
    r.active = false;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.stopping = true;
    }
    r.changed.notify_all();
    for (std::thread& t : r.threads) t.join();
    r.threads.clear();
    if (!r.failed) {
        if (r.format == Format::Gif) {
            std::vector<uint8_t> tail;
            r.gif.end(tail);
            r.file.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
        } else {
            r.avi.finishPart(r.file);
            std::vector<uint8_t> head = r.avi.header();
            const uint32_t riff = r.avi.riffSize();
            for (int i = 0; i < 4; ++i) head[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(riff >> (8 * i));
            r.file.seekp(0);
            r.file.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        }
        r.file.close();
        if (r.file.fail()) r.fail(trf("No se pudo escribir %s: ¿falta espacio en el disco?", r.path.c_str()));
    } else {
        r.file.close();
    }
    if (r.failed && error) *error = r.error;
    return !r.failed;
}

}  // namespace plumnin
