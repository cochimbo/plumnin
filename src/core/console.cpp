#include "console.h"

#include "i18n.h"
#include "miniz.h"

namespace plumnin {

namespace {

constexpr uint32_t kStateMagic = 0x534E4C50;  // "PLNS"
constexpr uint32_t kStateVersion = 1;
constexpr uint32_t kMaxStateSize = 64u << 20;

uint32_t tagId(const char (&tag)[5]) {
    return static_cast<uint32_t>(tag[0]) | static_cast<uint32_t>(tag[1]) << 8 | static_cast<uint32_t>(tag[2]) << 16 |
           static_cast<uint32_t>(tag[3]) << 24;
}

// Unpacks a savestate file into its body. Returns an empty string on success.
std::string unpackState(const std::vector<uint8_t>& in, std::vector<uint8_t>& body) {
    if (in.size() < 12) return tr("el archivo no es un estado de plumnin");
    StateReader header(in.data(), 12);
    uint32_t magic = 0, version = 0, rawSize = 0;
    header(magic, version, rawSize);
    if (magic != kStateMagic) return tr("el archivo no es un estado de plumnin");
    if (version > kStateVersion) return tr("el estado es de una versión más nueva de plumnin");
    if (rawSize == 0 || rawSize > kMaxStateSize) return tr("el estado está dañado");
    body.resize(rawSize);
    mz_ulong size = rawSize;
    if (mz_uncompress(body.data(), &size, in.data() + 12, static_cast<mz_ulong>(in.size() - 12)) != MZ_OK ||
        size != rawSize)
        return tr("el estado está dañado");
    return {};
}

}  // namespace

void Console::StateBlocks::raw(const char (&tag)[5], const std::vector<uint8_t>& data) {
    uint32_t id = tagId(tag), size = static_cast<uint32_t>(data.size());
    body_(id, size);
    body_.raw(data.data(), data.size());
}

bool Console::loadRom(const std::string& path, std::string* error) {
    std::vector<uint8_t> rom;
    if (!readRomFile(path, rom, nullptr, error)) return false;
    return loadRomData(std::move(rom), error);
}

bool Console::loadRomData(std::vector<uint8_t> rom, std::string* error) {
    loadNotice_.clear();
    if (rom.empty()) {
        if (error) *error = tr("el archivo está vacío");
        return false;
    }
    if (!insert(rom, error)) return false;
    romCrc_ = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, rom.data(), rom.size()));
    rom_ = std::move(rom);
    frames_ = 0;
    loadedThumbnail_.clear();
    audio_.clearSamples();
    return true;
}

void Console::setRegion(Region r) {
    if (r == region_) return;
    region_ = r;
    regionChanged();
}

uint32_t Console::checksum() {
    // FNV-1a of the memory the achievements read: where games keep what matters.
    uint32_t h = 2166136261u;
    for (const MemoryRegion& m : memoryMap())
        for (size_t i = 0; i < m.size; ++i) h = (h ^ m.data[i]) * 16777619u;
    return h;
}

bool Console::saveState(std::vector<uint8_t>& out, bool quick) {
    StateWriter body;
    uint32_t crc = romCrc_;
    uint8_t sys = static_cast<uint8_t>(system());
    uint8_t region = static_cast<uint8_t>(effectiveRegion());
    body(crc, sys, region, frames_);
    StateBlocks blocks(body);
    blocks.add("PADS", [&](StateWriter& w) { w(padTypes_); });
    saveBlocks(blocks);

    if (quick) {
        // No thumbnail: nobody looks at a rewind snapshot.
    } else if (!loadedThumbnail_.empty() && thumbnailFrame_ == frames_) {
        // Nothing has run since a state was loaded: the picture on screen is still the
        // one from before, so keep the loaded state's thumbnail.
        blocks.raw("THMB", loadedThumbnail_);
    } else {
        // Thumbnail: the 2D picture at half size, without the widescreen columns.
        const VideoFrame& f = frame();
        const int step = f.interlaced ? 2 : 1;
        const int scale = f.consoleWidth() > 320 ? 4 : 2;  // the SNES's 512 columns at a quarter
        uint16_t tw = static_cast<uint16_t>(f.consoleWidth() / scale), th = static_cast<uint16_t>(f.height / step / 2);
        StateWriter w;
        w(tw, th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const uint32_t c = f.pixel(f.wideLeft + x * scale, y * 2 * step) & 0xFFFFFF;
                const uint8_t rgb[3] = {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8),
                                        static_cast<uint8_t>(c)};
                w.raw(rgb, 3);
            }
        blocks.raw("THMB", w.data());
    }

    mz_ulong packedSize = mz_compressBound(static_cast<mz_ulong>(body.data().size()));
    std::vector<uint8_t> packed(packedSize);
    if (mz_compress2(packed.data(), &packedSize, body.data().data(), static_cast<mz_ulong>(body.data().size()),
                     quick ? 1 : 6) != MZ_OK)
        return false;
    packed.resize(packedSize);
    StateWriter file;
    uint32_t magic = kStateMagic, version = kStateVersion, rawSize = static_cast<uint32_t>(body.data().size());
    file(magic, version, rawSize);
    file.raw(packed.data(), packed.size());
    out = std::move(file.data());
    return true;
}

bool Console::loadState(const std::vector<uint8_t>& in, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    std::vector<uint8_t> body;
    const std::string problem = unpackState(in, body);
    if (!problem.empty()) return fail(problem);

    StateReader r(body.data(), body.size());
    uint32_t crc = 0;
    uint8_t sys = 0, region = 0;
    uint64_t frames = 0;
    r(crc, sys, region, frames);
    if (!r.ok()) return fail(tr("el estado está dañado"));
    if (sys != static_cast<uint8_t>(system())) return fail(tr("el estado es de otra consola"));
    if (crc != romCrc_) return fail(tr("el estado es de otra ROM"));

    // Keep the current state so a bad block cannot leave the console half loaded.
    std::vector<uint8_t> backup;
    saveState(backup, true);

    bool ok = true;
    std::vector<uint8_t> thumbnail;
    while (ok && !r.atEnd()) {
        uint32_t id = 0, size = 0;
        r(id, size);
        if (!r.ok() || size > r.remaining()) {
            ok = false;
            break;
        }
        std::vector<uint8_t> data(size);
        if (!r.raw(data.data(), size)) {
            ok = false;
            break;
        }
        StateReader b(data.data(), data.size());
        if (id == tagId("THMB")) thumbnail = std::move(data);
        else if (id == tagId("PADS")) b(padTypes_);
        else ok = loadBlock(id, b);
        if (ok && !b.ok()) ok = false;
    }
    if (!ok) {
        std::string ignored;
        if (!backup.empty()) {
            // Back to how it was: the backup is a state of this very console.
            loadState(backup, &ignored);
        }
        return fail(tr("el estado está dañado"));
    }
    frames_ = frames;
    afterLoad();
    loadedThumbnail_ = std::move(thumbnail);
    thumbnailFrame_ = frames_;
    return true;
}

bool Console::stateThumbnail(const std::vector<uint8_t>& state, std::vector<uint32_t>& pixels, int* width,
                             int* height) {
    std::vector<uint8_t> body;
    if (!unpackState(state, body).empty()) return false;
    StateReader r(body.data(), body.size());
    uint32_t crc = 0;
    uint8_t sys = 0, region = 0;
    uint64_t frames = 0;
    r(crc, sys, region, frames);
    while (r.ok() && !r.atEnd()) {
        uint32_t id = 0, size = 0;
        r(id, size);
        if (!r.ok() || size > r.remaining()) return false;
        std::vector<uint8_t> data(size);
        r.raw(data.data(), size);
        if (id != tagId("THMB")) continue;
        StateReader t(data.data(), data.size());
        uint16_t w = 0, h = 0;
        t(w, h);
        if (!t.ok() || static_cast<size_t>(w) * h * 3 != t.remaining() || w == 0 || h == 0) return false;
        pixels.resize(static_cast<size_t>(w) * h);
        for (uint32_t& p : pixels) {
            uint8_t rgb[3];
            t.raw(rgb, 3);
            p = 0xFF000000u | static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
        }
        *width = w;
        *height = h;
        return true;
    }
    return false;
}

bool Console::runAhead(int frames) {
    if (frames <= 0) return true;
    if (!saveState(aheadState_, true)) return false;
    const bool drawing = drawing_;
    for (int i = 1; i <= frames; ++i) {
        drawing_ = i == frames;  // only the last picture is seen
        runFrame();
    }
    drawing_ = drawing;
    // The frames to come are run again for real, and heard then.
    audio_.clearSamples();
    // The picture is not part of the state, so loading it keeps the one just drawn.
    std::string error;
    return loadState(aheadState_, &error);
}

}  // namespace plumnin
