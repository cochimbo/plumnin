#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/system.h"
#include "game_info_data.h"

namespace plumnin {

// What the library's detail panel shows of a game besides its cover: developer,
// publisher, genre, year and players from libretro-database's metadat files (one set
// per console, downloaded once and kept), and for the game picked its description and
// a screenshot from what ScreenScraper downloaded of it (media come only from there).
// A worker thread does the reading; update() hands its results to the main thread.
class GameInfo {
public:
    // The game in the panel.
    struct Sheet {
        std::string path;         // the ROM, empty while there is none
        bool busy = false;        // reading its description and screenshot
        bool known = false;       // ScreenScraper's media of it were downloaded
        std::string description;  // ScreenScraper's, empty if there is none
        std::string status;       // why there is no description, or empty
        unsigned int snap = 0;    // the screenshot's texture, 0 if there is none
        int width = 0, height = 0;
    };

    GameInfo();
    ~GameInfo();
    GameInfo(const GameInfo&) = delete;
    GameInfo& operator=(const GameInfo&) = delete;

    // Where the metadat files are kept (ending in a separator).
    void setFolder(const std::string& folder) { folder_ = folder; }
    // ScreenScraper's media folder (ScreenScraper::setFolder's).
    void setMediaFolder(const std::string& folder) { mediaFolder_ = folder; }

    // libretro-database's data of the console's games: loaded (or downloaded) the first
    // time it is asked for, so null until then.
    const GameDetails* details(ConsoleSystem s, const std::string& fileStem);
    // Changes each time a console's data arrives, to sort the list again.
    unsigned detailsVersion() const { return detailsVersion_; }
    bool detailsBusy() const;

    // Shows the game in the panel, reading its description and screenshot a moment
    // later (so that running through the grid does not read every game's).
    void show(const std::string& path, const std::string& stem, const std::string& title, ConsoleSystem s);
    // Reads the game at `path` again if it is the one in the panel (ScreenScraper has
    // just downloaded its media).
    void refresh(const std::string& path);
    const Sheet& sheet() const { return sheet_; }

    // Main thread, once a frame while the library is open.
    void update();
    void clearTextures();

private:
    struct Task {
        enum Kind { LoadDetails, LoadSheet } kind = LoadSheet;
        unsigned serial = 0;
        ConsoleSystem system = ConsoleSystem::GameBoy;
        std::string path, stem, title;
    };
    struct Reply {
        Task::Kind kind = Task::LoadSheet;
        unsigned serial = 0;
        ConsoleSystem system = ConsoleSystem::GameBoy;
        std::shared_ptr<DetailsTable> table;
        std::string description, status;
        bool known = false;
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };

    void work();
    void ask(Task task);
    std::shared_ptr<DetailsTable> loadDetails(ConsoleSystem s);
    void loadSheet(const Task& task, Reply& reply);
    std::string consoleFolder(ConsoleSystem s) const;

    std::string folder_;
    std::string mediaFolder_;
    std::shared_ptr<DetailsTable> tables_[kSystemCount];  // main thread
    bool asked_[kSystemCount] = {};
    std::chrono::steady_clock::time_point retryAt_[kSystemCount] = {};  // after a failed download
    unsigned detailsVersion_ = 0;
    int detailsPending_ = 0;

    Sheet sheet_;
    Task next_;  // the sheet to ask for once the selection settles
    bool hasNext_ = false;
    std::chrono::steady_clock::time_point nextAt_;
    unsigned serial_ = 0;  // of the latest sheet asked for; older replies are dropped

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;  // details first, then at most one sheet
    std::deque<Reply> replies_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace plumnin
