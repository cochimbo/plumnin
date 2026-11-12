#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct rc_client_t;
struct rc_client_event_t;
struct rc_api_request_t;
struct rc_api_server_response_t;

namespace plumnin {

class Console;

// RetroAchievements (retroachievements.org): the achievements, leaderboards and rich
// presence of the game being played, through rcheevos' rc_client. It identifies the game
// by the same hash as the other emulators, reads the console's memory as the
// achievements expect it, and makes the requests to the server on worker threads; their
// answers, and everything the UI is told, come back on the thread that calls poll().
// Hardcore mode is what the user switches on; while it is on (and they are logged in)
// the frontend must keep rewinding, slow motion, cheats and loading states off.
class Achievements {
public:
    // A blocking HTTP request: GET when `post` is empty, else POST of `post` (form data).
    // Returns the HTTP status (0 if the server could not be reached) and the body.
    using Http = std::function<int(const std::string& url, const std::string& post, const std::string& userAgent,
                                   std::string& body)>;

    // With `threaded` off the requests are made inside poll() instead (tests).
    explicit Achievements(Http http, bool threaded = true);
    ~Achievements();
    Achievements(const Achievements&) = delete;
    Achievements& operator=(const Achievements&) = delete;

    // "plumnin/<version> rcheevos/<version>", the User-Agent of every request.
    std::string userAgent() const;

    // Delivers the answers that arrived and lets the client retry, ping and so on. Call
    // once per frame (also while the game is paused or not loaded).
    void poll();

    // Account.
    void loginWithPassword(const std::string& user, const std::string& password);
    void loginWithToken(const std::string& user, const std::string& token);
    void logout();
    bool loggingIn() const { return loggingIn_; }
    bool loggedIn() const;
    std::string userName() const;     // as the site shows it
    std::string token() const;        // what to keep instead of the password
    uint32_t score() const;           // hardcore points
    const std::string& loginError() const { return loginError_; }

    // Hardcore mode: what the user chose. Switching it on with a game loaded makes the
    // game start again (a reset() request in the notices).
    void setHardcore(bool on);
    bool hardcore() const;

    // The game. `hash` is gameHash() of the console just loaded (before cheats change
    // its ROM). The game is identified after logging in, if that is still going on.
    void loadGame(Console& console, const std::string& hash);
    void unloadGame();
    bool gameLoaded() const;  // identified, with or without achievements
    bool loadingGame() const { return loadingGame_; }
    // The console was reset (by the user or because hardcore asked for it).
    void reset();
    // After every emulated frame, and in its place while nothing runs.
    void doFrame();
    void idle();
    // Savestates: the achievements' progress, kept next to the state. Restoring an empty
    // one starts every achievement over (a state from before plumnin kept them).
    std::vector<uint8_t> saveProgress();
    void loadProgress(const std::vector<uint8_t>& data);

    // The game's hash as RetroAchievements knows it: the MD5 of the cartridge's ROM, without the header a NES or SNES dump may carry.
    static std::string gameHash(Console& console);
    // The console's memory as the achievements address it (Console::memoryMap()). Returns
    // how many bytes could be read.
    static uint32_t readMemory(Console& console, uint32_t address, uint8_t* buffer, uint32_t count);

    // What to tell the user, in the order it happened.
    struct Notice {
        enum class Kind {
            Unlocked,       // an achievement (title, text, points, badge)
            GameStarted,    // the game was identified: how many achievements it has
            GameCompleted,  // every achievement earned
            Leaderboard,    // a leaderboard started, failed or was submitted
            Info,           // login, errors, connection lost or back
            Reset,          // the console must be reset (hardcore was switched on)
        };
        Kind kind = Kind::Info;
        std::string title;
        std::string text;
        std::string badgeUrl;
        uint32_t points = 0;
        bool error = false;
    };
    std::vector<Notice> takeNotices();

    // For the list of the game's achievements.
    struct Item {
        std::string title, description, badgeUrl;
        uint32_t points = 0;
        bool unlocked = false;
        std::string progress;  // "3/10", empty if it has none
        float percent = 0;
    };
    struct Group {
        std::string label;  // "Desbloqueados", "Bloqueados"... as rcheevos names them
        std::vector<Item> items;
    };
    std::vector<Group> achievementList();
    std::string gameTitle() const;
    std::string gameBadgeUrl() const;
    uint32_t achievementCount() const;
    uint32_t unlockedCount() const;
    uint32_t pointsTotal() const;
    uint32_t pointsUnlocked() const;
    std::string richPresence();

    // On screen while they last: the achievements that are one step from being earned
    // (challenges), the one making progress and the leaderboards' counters.
    struct Indicator {
        uint32_t id = 0;
        std::string badgeUrl, text;
    };
    const std::vector<Indicator>& challenges() const { return challenges_; }
    const Indicator* progress() const { return progressShown_ ? &progress_ : nullptr; }
    const std::vector<Indicator>& trackers() const { return trackers_; }

private:
    struct Request;
    struct Shared;
    static uint32_t readMemoryCallback(uint32_t address, uint8_t* buffer, uint32_t count, rc_client_t* client);
    static void serverCall(const rc_api_request_t* request, void (*callback)(const rc_api_server_response_t*, void*),
                           void* callbackData, rc_client_t* client);
    static void eventHandler(const rc_client_event_t* event, rc_client_t* client);
    static void loginDone(int result, const char* error, rc_client_t* client, void* userdata);
    static void gameDone(int result, const char* error, rc_client_t* client, void* userdata);
    void beginLoadGame();
    void notice(Notice::Kind kind, std::string title, std::string text = {}, std::string badge = {},
                uint32_t points = 0, bool error = false);

    Http http_;
    bool threaded_;
    std::shared_ptr<Shared> shared_;
    std::vector<std::unique_ptr<Request>> pending_;  // not threaded: made in the next poll()
    rc_client_t* client_ = nullptr;
    Console* console_ = nullptr;
    std::string hash_;
    bool wantGame_ = false;  // a game is loaded, waiting for the login to be identified
    bool loggingIn_ = false;
    bool loadingGame_ = false;
    std::string loginError_;
    std::vector<Notice> notices_;
    std::vector<Indicator> challenges_;
    Indicator progress_;
    bool progressShown_ = false;
    std::vector<Indicator> trackers_;
};

}  // namespace plumnin
