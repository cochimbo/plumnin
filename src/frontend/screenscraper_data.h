#pragma once

#include <string>
#include <utility>
#include <vector>

namespace plumnin {

// What ScreenScraper (screenscraper.fr) answers and how to ask it, without SDL or a
// network, so the tests can use it. The API (v2) is asked for JSON:
//   jeuInfos.php     one game, by the ROM's CRC32, name and size, or by its id (gameid)
//   jeuRecherche.php up to 30 games whose names look like a text (recherche)
// Every request carries the program's developer account (devid, devpassword,
// softname) and, if the user has one, theirs (ssid, sspassword), which gives them more
// requests a day and threads.

// A tiny JSON tree: enough for ScreenScraper's answers, where numbers come as strings
// anyway. Tolerates the trailing commas the server sometimes sends.
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string text;               // a string, number or bool as written ("true")
    std::vector<std::string> keys;  // an object's, in order
    std::vector<JsonValue> items;   // an array's elements, or an object's values

    // A member of an object (a Null value if there is none, or this is not an object).
    const JsonValue& operator[](const char* key) const;
    bool isObject() const { return type == Object; }
    bool isArray() const { return type == Array; }
};
// False if `text` is not JSON (an error page, a message in plain text...).
bool parseJson(const std::string& text, JsonValue& out);
// `s` as a JSON string, with quotes.
std::string jsonQuote(const std::string& s);

// ScreenScraper's id of a console: 9 Game Boy, 10 Game Boy Color, 3 NES, 4 Super
// Nintendo. `system` is a ConsoleSystem.
int screenScraperSystem(int system);

struct SsMedia {
    std::string type;    // "box-2D", "ss", "wheel-hd"...
    std::string region;  // "eu", "us", "jp", "wor"..., empty for the ones without
    std::string format;  // "png", "jpg", "mp4", "pdf"
    std::string url;     // without the accounts (see ssStripCredentials)
};

// One game as ScreenScraper knows it.
struct SsGame {
    int id = 0;
    int system = 0;           // ScreenScraper's id of its console
    std::string systemName;
    std::vector<std::pair<std::string, std::string>> names;     // region, name
    std::vector<std::pair<std::string, std::string>> dates;     // region, "1991-06-23" or "1991"
    std::vector<std::pair<std::string, std::string>> synopsis;  // language, text
    std::vector<std::vector<std::pair<std::string, std::string>>> genres;  // each one's names: language, name
    std::string publisher, developer, players, rating;  // rating out of 20
    std::vector<SsMedia> medias;

    // In the first of `regions` (or languages) that it has, else the first it has.
    std::string name(const std::vector<std::string>& regions) const;
    std::string date(const std::vector<std::string>& regions) const;
    std::string text(const std::vector<std::string>& languages) const;
    std::string genre(const std::vector<std::string>& languages) const;  // all of them, with commas
    std::string year(const std::vector<std::string>& regions) const { return date(regions).substr(0, 4); }
};

// An answer of the API.
struct SsAnswer {
    enum Result {
        Ok,
        NotFound,     // 404: no game has that ROM (or that name)
        Offline,      // the server could not be reached
        Login,        // 401/403: an account is wrong (the developer's or the user's)
        Busy,         // 429 or "closed for non-members": too many requests at once, try again
        Quota,        // 430/431: the day's requests (or failed ones) are used up
        Closed,       // 423: the API is closed for a while
        Error         // anything else, with `message`
    } result = Error;
    std::string message;        // what the server said, for the user
    std::vector<SsGame> games;  // one for jeuInfos, the list for jeuRecherche
    int requestsToday = -1, maxRequests = -1;  // the user's, when it says
};
SsAnswer parseSsAnswer(int httpStatus, const std::string& body);

// The answer for a single game, as plumnin keeps it next to its media (the same shape
// as jeuInfos', so parseSsAnswer reads it back).
std::string ssGameJson(const SsGame& game);

// The kinds of media plumnin asks for, in the order the media window shows them. `types`
// are ScreenScraper's names, the best first.
struct SsKind {
    const char* id;     // the file's name in the game's folder ("portada.png")
    const char* label;  // in Spanish, for tr()
    const char* types[3];
    bool image;      // false: a video or a PDF, opened with the system's program
    bool byDefault;  // downloaded unless the user says otherwise (videos and manuals are big)
};
const std::vector<SsKind>& ssKinds();
const SsKind* ssKind(const std::string& id);

// Region codes to prefer for a ROM, from its tags as gameTags() gives them
// ("USA · Europe"): its own regions first, then world, Europe, USA, Japan and
// ScreenScraper's own ("ss").
std::vector<std::string> ssRegions(const std::string& tags);
// Languages to prefer for texts: `ui` ("es") first, then English and French.
std::vector<std::string> ssLanguages(const std::string& ui);

// A game's folder under ScreenScraper's media folder `root`: <root>/<console>/<ROM
// name>/, ending in a separator ("" if `root` is). `system` is a ConsoleSystem.
std::string ssGameFolder(const std::string& root, int system, const std::string& stem);

// The media of `kind` that `game` has, in the best region; null if none.
const SsMedia* ssPickMedia(const SsGame& game, const SsKind& kind, const std::vector<std::string>& regions);

// The accounts of a request.
struct SsCredentials {
    std::string devId, devPassword, softName;  // the program's
    std::string user, password;                 // the user's, may be empty
    bool usable() const { return !devId.empty() && !devPassword.empty(); }
};
// https://api.screenscraper.fr/api2/<endpoint>?<accounts>&output=json&<params>
// (`params` already percent-encoded, without a leading '&').
std::string ssUrl(const SsCredentials& c, const std::string& endpoint, const std::string& params);
// `url` without devid, devpassword, softname, ssid and sspassword (to keep it on disk),
// and with the ones of `c` (to download it).
std::string ssStripCredentials(const std::string& url);
std::string ssWithCredentials(const std::string& url, const SsCredentials& c);
// The query of jeuInfos for a ROM: by its CRC32 (0: not known, a disc), file name and size.
std::string ssRomParams(int system, const std::string& fileName, unsigned long long size, unsigned crc, bool disc);

}  // namespace plumnin
