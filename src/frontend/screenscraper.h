#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/system.h"
#include "screenscraper_data.h"

namespace plumnin {

// The media of the library's games from ScreenScraper (screenscraper.fr): covers, the
// whole sleeve, the cartridge's label, screenshots, logos... and its data of each game.
// A game is identified by its ROM (CRC32, name and size; discs by their name), or by
// the name if that fails, or the user picks it from a search. What is downloaded goes
// to <folder>/<console>/<ROM name>/, one file per kind (portada.png, etiqueta.png...)
// and ficha.json with what ScreenScraper knows of it; the cover and the label are also
// handed to the library (takeForLibrary), which keeps them where it looks for them.
// One worker thread makes the requests one after another (a free account has one
// thread); update() hands what it found to the main thread.
class ScreenScraper {
public:
    struct Rom {
        std::string path, stem, title, tags;
        ConsoleSystem system = ConsoleSystem::GameBoy;
    };
    // One downloaded file of the game in the window.
    struct Medium {
        std::string kind;   // SsKind::id
        std::string file;
        unsigned int texture = 0;  // 0 for a video or a manual, or an image that would not decode
        int width = 0, height = 0;
    };
    // The game in the media window.
    struct Sheet {
        std::string path;        // its ROM, empty while there is none
        bool busy = false;       // reading its folder or downloading
        std::string progress;    // what is being downloaded
        std::string status;      // the last result or error
        bool known = false;      // it has a ficha.json
        SsGame game;
        std::vector<Medium> media;
    };
    // A search by name, to pick the right game by hand.
    struct Search {
        bool busy = false;
        std::string status;
        std::vector<SsGame> results;
        int previewId = 0;  // the result whose cover is in `preview`
        unsigned int preview = 0;
        int width = 0, height = 0;
        bool previewBusy = false;
    };
    // A cover (or whole sleeve) and a label for the library, to save as the game's.
    struct ForLibrary {
        std::string path;
        std::vector<uint8_t> cover, label;
        bool replace = false;  // asked for by hand: over what the game has
    };

    ScreenScraper();
    ~ScreenScraper();
    ScreenScraper(const ScreenScraper&) = delete;
    ScreenScraper& operator=(const ScreenScraper&) = delete;

    // Where the media go (ending in a separator).
    void setFolder(const std::string& folder) { folder_ = folder; }
    void setCredentials(const SsCredentials& c);
    SsCredentials credentials() const;
    // The developer account plumnin was built with (empty if none).
    static SsCredentials builtIn();
    // Which kinds to download (SsKind ids); empty: the ones with byDefault.
    void setKinds(std::vector<std::string> kinds);
    std::string gameFolder(const Rom& rom) const;
    // Downloading again: every chosen file anew (true), or only those it has not got yet,
    // from the game's ficha.json if it has one, without asking ScreenScraper for it again.
    void setOverwrite(bool overwrite) { overwrite_ = overwrite; }
    // The language of the texts ("es").
    void setLanguage(const std::string& language) { language_ = language; }

    // Shows the game's media in the window, read from its folder.
    void show(const Rom& rom);
    const Sheet& sheet() const { return sheet_; }
    // Downloads what ScreenScraper has of the game: the one with `gameId` (from a
    // search), or the one its ROM is (0). `kinds` empty: the chosen kinds.
    void download(const Rom& rom, int gameId = 0, std::vector<std::string> kinds = {});
    void search(const std::string& text, ConsoleSystem s);
    void preview(const SsGame& game, const std::vector<std::string>& regions);
    const Search& searchState() const { return search_; }
    // Forgets the window's game and search, and frees their textures.
    void close();

    // Every game of `roms` one after another: with overwrite, all of them; else those
    // that have no ficha.json yet or miss some of the chosen kinds that ScreenScraper has.
    void downloadAll(const std::vector<Rom>& roms, bool overwrite);
    void cancelAll();
    int batchLeft() const;
    int batchTotal() const { return batchTotal_; }
    std::string batchStatus() const;

    bool takeForLibrary(ForLibrary& out);

    // "Probar la conexión": asks ScreenScraper about the accounts in use (the user's, if
    // there is one: its requests today), and keeps what it said.
    void check();
    bool checking() const { return checking_; }
    const std::string& checkStatus() const { return checkStatus_; }
    bool checkOk() const { return checkOk_; }
    // The round of downloads stopped on an error (batchStatus() says which).
    bool batchFailed() const;

    // Main thread, once a frame.
    void update();
    void clearTextures();

private:
    struct Task {
        enum Kind { Show, Download, Search, Preview, Batch, Check } kind = Show;
        unsigned serial = 0;
        Rom rom;
        int gameId = 0;
        bool overwrite = false;
        std::vector<std::string> kinds;
        std::string text;
        ConsoleSystem system = ConsoleSystem::GameBoy;
        std::string url;  // a preview's
    };
    struct Image {
        std::string kind, file;
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };
    struct Reply {
        Task::Kind kind = Task::Show;
        unsigned serial = 0;
        std::string path;
        std::string status;
        bool known = false;
        SsGame game;
        std::vector<Image> images;
        std::vector<SsGame> results;
        int previewId = 0;
        bool ok = false;
    };

    void work();
    void queue(Task task, bool first);
    void post(Reply reply);
    void setProgress(const std::string& text);
    // The API, throttled and retried while the server is busy.
    SsAnswer ask(const std::string& endpoint, const std::string& params);
    // Identifies and downloads one game; false with *error if it could not.
    bool fetchGame(const Task& task, std::string* error, bool batch);
    void readFolder(const Rom& rom, Reply& reply) const;
    std::vector<std::string> chosenKinds(const std::vector<std::string>& asked) const;
    // The chosen kinds the game's folder has not got and its ficha.json says ScreenScraper
    // has (all of them if there is no ficha.json); `known` says whether there was one.
    std::vector<std::string> missingKinds(const Rom& rom, bool* known) const;

    std::string folder_;
    std::string language_ = "es";
    bool overwrite_ = false;
    unsigned serial_ = 0;  // of the window's game; replies for another one are dropped
    unsigned searchSerial_ = 0;
    Sheet sheet_;
    Search search_;
    int batchTotal_ = 0;
    bool checking_ = false, checkOk_ = false;  // main thread
    std::string checkStatus_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;
    std::deque<Reply> replies_;
    std::deque<ForLibrary> forLibrary_;
    SsCredentials credentials_;
    std::vector<std::string> kinds_;
    std::string progress_;
    std::string shown_;         // the window's game, for the round of downloads
    unsigned shownSerial_ = 0;
    int batchLeft_ = 0, batchDone_ = 0, batchMissed_ = 0;
    std::string batchError_;  // what stopped the round
    bool stop_ = false;
    long long lastRequest_ = 0;  // steady clock, ms
    std::thread worker_;
};

}  // namespace plumnin
