#include "game_info.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "../core/i18n.h"
#include "../render/gl.h"
#include "http.h"
#include "library.h"
#include "library_names.h"
#include "screenscraper_data.h"

namespace plumnin {

namespace fs = std::filesystem;

namespace {

// Each console's folder for what is downloaded, and its name in libretro-database.
struct InfoSet {
    const char* folder;
    const char* database;
};
constexpr InfoSet kInfoSets[] = {
    {"GameBoy", "Nintendo - Game Boy"},
    {"GameBoyColor", "Nintendo - Game Boy Color"},
    {"NES", "Nintendo - Nintendo Entertainment System"},
    {"SNES", "Nintendo - Super Nintendo Entertainment System"},
};
static_assert(std::size(kInfoSets) == kSystemCount, "one set per console");

const char* const kFields[] = {"developer", "publisher", "genre", "releaseyear", "maxusers"};
const char* const kMetadat = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/";
constexpr auto kSettle = std::chrono::milliseconds(300);  // before looking for a game picked
constexpr auto kRetry = std::chrono::minutes(2);          // before downloading a console's data again

bool readText(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Texts for the user, translated on the main thread as they are shown.
const char* const kNotDownloaded = N_("Descarga sus medios de ScreenScraper para ver su descripción y una captura.");
const char* const kNoText = N_("ScreenScraper no tiene descripción de este juego.");

}  // namespace

GameInfo::GameInfo() : worker_(&GameInfo::work, this) {}

GameInfo::~GameInfo() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
}

std::string GameInfo::consoleFolder(ConsoleSystem s) const {
    return (fs::u8path(folder_) / kInfoSets[static_cast<int>(s)].folder).u8string() +
           static_cast<char>(fs::path::preferred_separator);
}

void GameInfo::ask(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task.kind == Task::LoadSheet)
            for (auto it = tasks_.begin(); it != tasks_.end();)
                it = it->kind == Task::LoadSheet ? tasks_.erase(it) : it + 1;
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

const GameDetails* GameInfo::details(ConsoleSystem s, const std::string& fileStem) {
    const int i = static_cast<int>(s);
    if (!kInfoSets[i].database || folder_.empty()) return nullptr;
    if (!tables_[i]) {
        const auto now = std::chrono::steady_clock::now();
        if (!asked_[i] && now >= retryAt_[i]) {
            asked_[i] = true;
            ++detailsPending_;
            Task t;
            t.kind = Task::LoadDetails;
            t.system = s;
            ask(std::move(t));
        }
        return nullptr;
    }
    return tables_[i]->find(fileStem);
}

void GameInfo::show(const std::string& path, const std::string& stem, const std::string& title, ConsoleSystem s) {
    if (path == sheet_.path) return;
    clearTextures();
    sheet_ = Sheet();
    sheet_.path = path;
    ++serial_;  // a reply on its way for another game is dropped
    if (path.empty() || folder_.empty()) {
        hasNext_ = false;
        return;
    }
    sheet_.busy = true;
    next_ = Task();
    next_.kind = Task::LoadSheet;
    next_.system = s;
    next_.path = path;
    next_.stem = stem;
    next_.title = title;
    hasNext_ = true;
    nextAt_ = std::chrono::steady_clock::now() + kSettle;
}

void GameInfo::refresh(const std::string& path) {
    if (path.empty() || path != sheet_.path || hasNext_) return;  // not shown, or about to be read anyway
    Task t = next_;
    if (t.path != path) return;
    sheet_.busy = true;
    t.serial = ++serial_;
    ask(std::move(t));
}

bool GameInfo::detailsBusy() const { return detailsPending_ > 0; }

void GameInfo::update() {
    if (hasNext_ && std::chrono::steady_clock::now() >= nextAt_) {
        hasNext_ = false;
        Task t = next_;
        t.serial = serial_;
        ask(std::move(t));
    }
    for (;;) {
        Reply r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (replies_.empty()) return;
            r = std::move(replies_.front());
            replies_.pop_front();
        }
        const int i = static_cast<int>(r.system);
        if (r.kind == Task::LoadDetails) {
            --detailsPending_;
            if (r.table) {
                tables_[i] = std::move(r.table);
                ++detailsVersion_;
            } else {
                asked_[i] = false;  // try again in a while
                retryAt_[i] = std::chrono::steady_clock::now() + kRetry;
            }
            continue;
        }
        if (r.serial != serial_) continue;
        clearTextures();
        sheet_.busy = false;
        sheet_.known = r.known;
        sheet_.description = std::move(r.description);
        sheet_.status = r.status.empty() ? std::string() : std::string(tr(r.status.c_str()));
        if (!r.pixels.empty()) {
            sheet_.snap = Library::uploadTexture(sheet_.snap, r.pixels, r.width, r.height);
            sheet_.width = r.width;
            sheet_.height = r.height;
        }
    }
}

void GameInfo::clearTextures() {
    if (sheet_.snap) gl::DeleteTextures(1, &sheet_.snap);
    sheet_.snap = 0;
    sheet_.width = sheet_.height = 0;
}

// The console's metadat files, from the folder or downloaded into it.
std::shared_ptr<DetailsTable> GameInfo::loadDetails(ConsoleSystem s) {
    const char* database = kInfoSets[static_cast<int>(s)].database;
    const fs::path dir = fs::u8path(consoleFolder(s));
    auto table = std::make_shared<DetailsTable>();
    for (const char* field : kFields) {
        const fs::path file = dir / (std::string(field) + ".dat");
        std::string text;
        if (!readText(file, text) || text.empty()) {
            std::vector<uint8_t> body;
            const std::string url = std::string(kMetadat) + field + "/" + urlEncode(database) + ".dat";
            if (httpGet(url, body) != 200 || body.empty()) return nullptr;
            text.assign(body.begin(), body.end());
            writeFile(file, text.data(), text.size());
        }
        table->add(field, text);
    }
    return table;
}

// The game's description and screenshot, from what ScreenScraper downloaded of it (its
// ficha.json and captura.png, else the title screen). Nothing is downloaded here.
void GameInfo::loadSheet(const Task& task, Reply& r) {
    const std::string dir = ssGameFolder(mediaFolder_, static_cast<int>(task.system), task.stem);
    std::string text;
    if (dir.empty() || !readText(fs::u8path(dir + "ficha.json"), text)) {
        r.status = kNotDownloaded;
        return;
    }
    r.known = true;
    const SsAnswer a = parseSsAnswer(200, text);
    if (!a.games.empty()) r.description = trimmed(a.games[0].text(ssLanguages(language())));
    if (r.description.empty()) r.status = kNoText;
    for (const char* kind : {"captura", "titulo"})
        for (const char* ext : {".png", ".jpg"}) {
            std::string image;
            if (!readText(fs::u8path(dir + kind + ext), image)) continue;
            const std::vector<uint8_t> data(image.begin(), image.end());
            if (Library::decodeImage(data, r.pixels, &r.width, &r.height)) return;
            r.pixels.clear();
        }
}

void GameInfo::work() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        Reply r;
        r.kind = task.kind;
        r.serial = task.serial;
        r.system = task.system;
        if (task.kind == Task::LoadDetails) r.table = loadDetails(task.system);
        else loadSheet(task, r);
        std::lock_guard<std::mutex> lock(mutex_);
        replies_.push_back(std::move(r));
    }
}

}  // namespace plumnin
