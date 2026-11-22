#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plumnin {

// A game's manual (the PDF ScreenScraper has of it), read a page at a time for the 3D
// view. The pages are drawn by PDFium (pdfium.dll next to plumnin, loaded the first time a
// manual is opened) on a thread of its own, and handed to OpenGL on the main thread in
// update(); only the pages around the ones asked for are kept.
class ManualReader {
public:
    struct Page {
        unsigned int texture = 0;  // 0 while it is being drawn
        int width = 0, height = 0;
    };

    ManualReader();
    ~ManualReader();
    ManualReader(const ManualReader&) = delete;
    ManualReader& operator=(const ManualReader&) = delete;

    // Opens the PDF at `path` ("" closes it). Another one than before forgets its pages.
    void open(const std::string& path);
    const std::string& path() const { return path_; }
    // How many pages it has: -1 while it is being opened, 0 if it could not be (error()).
    int pageCount() const { return pages_; }
    const std::string& error() const { return error_; }
    // The pages wanted now, first to last (they may be out of the manual); the rest go.
    void want(int first, int last);
    // A page, as far as it has been drawn (its size known once the manual is open).
    Page page(int index) const;
    // Main thread, once a frame: what the thread has drawn becomes textures.
    void update();
    // Frees the textures (before the OpenGL context goes).
    void clearTextures();

    // Whether PDFium can be loaded at all.
    static bool available();

private:
    struct Drawn {
        unsigned serial = 0;
        int index = -1;  // -1: the answer to opening it
        int count = 0;
        std::string error;
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };
    void work();

    std::string path_;
    int pages_ = -1;
    std::string error_;
    std::map<int, Page> textures_;
    int first_ = 0, last_ = -1;

    std::mutex mutex_;
    std::condition_variable wake_;
    unsigned serial_ = 0;       // of the manual open now
    std::string toOpen_;        // for the thread
    bool openPending_ = false;
    std::deque<int> toDraw_;    // pages the thread draws, first first
    std::deque<Drawn> drawn_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace plumnin
