#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/system.h"
#include "texture_cache.h"

namespace plumnin {

// The game library: the games of one or more folders (and their subfolders), each with
// the console it is for (Game Boy, Game Boy Color, NES or Super Nintendo) and its box art.
// Covers are looked for in the user's own covers folder, next to the ROMs (same name
// .png/.jpg, or in a caratulas, covers or boxart folder there) and in what was
// downloaded from ScreenScraper, the only place plumnin downloads them from (see
// ScreenScraper). A few worker threads find and decode the covers (or read them back,
// already decoded and shrunk, from the TextureCache); update() turns the decoded ones
// into OpenGL textures on the main thread.
class Library {
public:
    // For the detail panel's screenshots: a cover's decoding (shrunk to the covers'
    // height) and its upload as a texture (into `texture` if not 0; returns it).
    static bool decodeImage(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height);
    static unsigned int uploadTexture(unsigned int texture, const std::vector<uint32_t>& pixels, int width, int height);

    enum class Cover { Unknown, Pending, Missing, Ready };
    struct Game {
        std::string path;   // the ROM
        std::string stem;   // its file name without extension
        std::string title;  // the name to show
        std::string tags;   // region, revision...
        std::string key;    // searchKey() of title and tags
        ConsoleSystem system = ConsoleSystem::GameBoy;
        Cover cover = Cover::Unknown;
        unsigned int texture = 0;
        int width = 0, height = 0;
        // The cartridge's label: 0 if there is none.
        unsigned int label = 0;
        int labelWidth = 0, labelHeight = 0;
    };

    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Where the games' own covers go (ending in a separator): the ones picked by hand
    // and copies of ScreenScraper's.
    void setCoversFolder(const std::string& folder) { coversFolder_ = folder; }
    // ScreenScraper's media folder (ScreenScraper::setFolder's): a game's cover and label
    // are looked for there too, last.
    void setMediaFolder(const std::string& folder) { mediaFolder_ = folder; }
    // Where the decoded covers are kept between runs (ending in a separator), see
    // TextureCache. Set before scan().
    void setCacheFolder(const std::string& folder) { cache_.setFolder(folder); }
    // Lists the games of `folders` and starts looking for their covers.
    void scan(const std::vector<std::string>& folders);
    const std::vector<std::string>& folders() const { return folders_; }
    bool scanned() const { return scanned_; }
    std::vector<Game>& games() { return games_; }

    // How many covers are still being looked for.
    int pending() const { return pending_.load(); }
    // Looks for the covers of these games (indices into games()) before the others',
    // the first one first: the ones on screen.
    void prioritize(const std::vector<size_t>& games);

    // Uses an image of the user's own (PNG or JPEG, "Carátula desde imagen..." in its
    // menu) as `game`'s cover, copied to the covers folder like a downloaded one.
    // False, with the reason in *error, if it is not an image or cannot be saved.
    bool coverFromFile(size_t game, const std::string& file, std::string* error);
    // Uses an image of the user's as the label on `game`'s cartridge, saved
    // in the covers folder's "etiquetas" folder, where labels are looked for first; then
    // in an "etiquetas", "labels" or "Named_Labels" folder next to the game.
    bool labelFromFile(size_t game, const std::string& file, std::string* error);
    // Uses images downloaded for the game at `path` (ScreenScraper's): a front cover and
    // a label, either may be empty. Unless `replace`, only where the game has none, or
    // (the cover) where it has a whole sleeve, much wider than the front.
    void useDownloaded(const std::string& path, const std::vector<uint8_t>& cover, const std::vector<uint8_t>& label, bool replace);
    // Main thread, once a frame: uploads as many decoded covers as textures as fit in
    // `budgetMs` milliseconds (at least one).
    void update(int budgetMs = 3);
    void clearTextures();

private:
    struct Job {
        size_t index;
        unsigned generation;
        std::string path, stem;
        ConsoleSystem system;
    };
    struct Result {
        size_t index;
        unsigned generation;
        bool found;
        std::vector<uint32_t> pixels;
        int width, height;
        std::vector<uint32_t> label;
        int labelWidth = 0, labelHeight = 0;
    };

    void work();
    // Reads and decodes the image at `path`, from the cache if it is there; *key is
    // its key in the cache (0 if it has none).
    bool loadImage(const std::string& path, std::vector<uint32_t>& pixels, int* width, int* height, uint64_t* key) const;
    bool findLocal(const Job& job, std::string* coverPath) const;
    bool findLabel(const Job& job, std::string* labelPath) const;
    // The first of `kinds` (portada.png...) in the game's ScreenScraper folder.
    bool findMedia(const Job& job, std::initializer_list<const char*> kinds, std::string* path) const;
    void queue(Job job);
    // Saves a PNG or JPEG as `game`'s cover, replacing any other one of its in the covers
    // folder, and loads it again.
    bool saveCover(size_t game, const std::vector<uint8_t>& image, std::string* error, bool label = false);

    std::string systemCovers(ConsoleSystem s) const;

    std::vector<std::string> folders_;
    std::string coversFolder_;
    std::string mediaFolder_;
    bool scanned_ = false;
    std::vector<Game> games_;
    unsigned generation_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    std::atomic<int> pending_{0};
    bool stop_ = false;
    std::vector<std::thread> workers_;
    TextureCache cache_;
    // The cache's images this scan used: once all its covers are in, the rest are deleted.
    std::vector<uint64_t> usedKeys_;
    bool pruned_ = true;
};

}  // namespace plumnin
