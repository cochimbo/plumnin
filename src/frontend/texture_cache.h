#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumnin {

// The library's covers and labels as they are once decoded and shrunk, kept on disk so
// that the next start does not decode every PNG and JPEG again: a 1061 x 680 sleeve from
// ScreenScraper takes some 25 ms to decode as a PNG and a few to read back from here.
//
// Each image is one file in the cache folder, named by a hash of where it came from (its
// path, size and modification time, and how it was shrunk): a cover replaced by another
// one, or touched, simply gets a new file. The pixels are stored losslessly in the QOI
// format (https://qoiformat.org), which is about as small as a PNG and several times
// quicker to decode, after a small header of plumnin's own.
class TextureCache {
public:
    // `folder` ends in a separator; empty turns the cache off.
    void setFolder(const std::string& folder) { folder_ = folder; }
    const std::string& folder() const { return folder_; }

    // The key of the image file `path` once shrunk by `variant` (what decides how), 0 if
    // it cannot be read.
    static uint64_t key(const std::string& path, uint32_t variant);

    // The cached pixels for `key` (0xAABBGGRR), false if there are none.
    bool load(uint64_t key, std::vector<uint32_t>& pixels, int* width, int* height) const;
    // Keeps them for next time (written to a temporary file first, so a half-written one
    // is never read).
    void store(uint64_t key, const std::vector<uint32_t>& pixels, int width, int height) const;
    // Deletes the cached images whose keys are not in `keep` (sorted): the ones of covers
    // that were replaced or of games no longer in the library.
    void prune(const std::vector<uint64_t>& keep) const;

    // QOI, from and to 0xAABBGGRR pixels (memory order R, G, B, A).
    static std::vector<uint8_t> encodeQoi(const std::vector<uint32_t>& pixels, int width, int height);
    static bool decodeQoi(const uint8_t* data, size_t size, std::vector<uint32_t>& pixels, int* width, int* height);

private:
    std::string file(uint64_t key) const;
    std::string folder_;
};

}  // namespace plumnin
