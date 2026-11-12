#include "cheat_database.h"

#include "http.h"
#include "../core/i18n.h"
#include "library_names.h"

#include <iterator>

namespace plumnin {

namespace {

// libretro-database's cheat lists of each console: the files on GitHub and GitHub's
// tree of the folder for their names.
struct CheatSet {
    const char* files;
    const char* list;
};
constexpr CheatSet kCheatSets[] = {
    {"https://raw.githubusercontent.com/libretro/libretro-database/master/cht/Nintendo%20-%20Game%20Boy/",
     "https://api.github.com/repos/libretro/libretro-database/git/trees/master:cht/Nintendo%20-%20Game%20Boy"},
    {"https://raw.githubusercontent.com/libretro/libretro-database/master/cht/Nintendo%20-%20Game%20Boy%20Color/",
     "https://api.github.com/repos/libretro/libretro-database/git/trees/master:cht/Nintendo%20-%20Game%20Boy%20Color"},
    {"https://raw.githubusercontent.com/libretro/libretro-database/master/cht/Nintendo%20-%20Nintendo%20Entertainment%20System/",
     "https://api.github.com/repos/libretro/libretro-database/git/trees/master:cht/Nintendo%20-%20Nintendo%20Entertainment%20System"},
    {"https://raw.githubusercontent.com/libretro/libretro-database/master/cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System/",
     "https://api.github.com/repos/libretro/libretro-database/git/trees/master:cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System"},
};
static_assert(std::size(kCheatSets) == kSystemCount, "one set per console");

const CheatSet& cheatSet(ConsoleSystem s) { return kCheatSets[static_cast<int>(s)]; }

std::string fileStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// Texts for the user, translated on the worker thread as they are handed over.
const char* const kOffline = N_("No se pudo conectar con la base de trucos de libretro.");

}  // namespace

CheatDatabase::CheatDatabase() : worker_(&CheatDatabase::work, this) {}

CheatDatabase::~CheatDatabase() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
}

void CheatDatabase::ask(Task task) {
    task.serial = ++serial_;
    view_.busy = true;
    view_.status.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_ = std::move(task);
        hasTask_ = true;
    }
    wake_.notify_one();
}

void CheatDatabase::search(const std::string& path) {
    view_ = View();
    view_.game = path;
    system_ = detectSystem(path);
    Task t;
    t.search = true;
    t.system = system_;
    t.path = path;
    ask(std::move(t));
}

void CheatDatabase::load(const std::string& file) {
    if (file == view_.file && !view_.cheats.empty()) return;
    view_.file = file;
    view_.cheats.clear();
    Task t;
    t.system = system_;
    t.file = file;
    ask(std::move(t));
}

void CheatDatabase::clear() {
    ++serial_;  // a reply still on its way is dropped
    view_ = View();
}

void CheatDatabase::update() {
    Reply r;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasReply_) return;
        r = std::move(reply_);
        hasReply_ = false;
    }
    if (r.serial != serial_) return;
    view_.busy = false;
    view_.status = r.status;
    if (r.search) view_.files = std::move(r.files);
    view_.file = r.file;
    view_.cheats = std::move(r.cheats);
}

// The names in the console's cht folder. Kept for the rest of the session.
bool CheatDatabase::loadIndex(ConsoleSystem s, std::string* error) {
    std::vector<std::string>& index = index_[static_cast<int>(s)];
    if (!index.empty()) return true;
    std::vector<uint8_t> body;
    const int status = httpGet(cheatSet(s).list, body);
    if (status == 200) index = parseFileList(std::string(body.begin(), body.end()), ".cht");
    if (!index.empty()) return true;
    *error = status == 0 ? std::string(tr(kOffline)) : trf("No se pudo leer la lista de trucos de libretro (error %d).", status);
    return false;
}

bool CheatDatabase::download(ConsoleSystem s, const std::string& file, std::vector<Cheat>* out, int* status) {
    std::vector<uint8_t> body;
    *status = httpGet(cheatSet(s).files + urlEncode(file) + ".cht", body);
    if (*status != 200) return false;
    *out = parseChtFile(std::string(body.begin(), body.end()));
    return true;
}

void CheatDatabase::work() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || hasTask_; });
            if (stop_) return;
            task = std::move(task_);
            hasTask_ = false;
        }
        Reply r;
        r.serial = task.serial;
        r.search = task.search;
        int status = 0;
        if (task.search) {
            const std::string stem = fileStem(task.path);
            std::string indexError;
            if (loadIndex(task.system, &indexError)) {
                r.files = cheatFileNames(index_[static_cast<int>(task.system)], stem, 20);
                if (r.files.empty()) r.status = tr("La base de trucos de libretro no tiene listas para este juego.");
                else if (!download(task.system, r.files.front(), &r.cheats, &status))
                    r.status = status == 0 ? tr(kOffline) : tr("No se pudo descargar la lista de trucos. ¿Hay conexión a internet?");
                if (!r.files.empty()) r.file = r.files.front();
            } else {
                // Without the list of names (GitHub limits how often it is asked for),
                // the lists named exactly like the ROM can still be tried one by one.
                for (const std::string& n : coverNames(stem)) {
                    for (const std::string& name : {n, n + " (Action Replay)"}) {
                        std::vector<Cheat> cheats;
                        if (download(task.system, name, &cheats, &status)) {
                            r.files.push_back(name);
                            if (r.file.empty()) {
                                r.file = name;
                                r.cheats = std::move(cheats);
                            }
                        } else if (status == 0) {
                            break;
                        }
                    }
                    if (status == 0) break;
                }
                if (r.files.empty()) r.status = status == 0 ? std::string(tr(kOffline)) : indexError;
            }
        } else {
            r.file = task.file;
            if (!download(task.system, task.file, &r.cheats, &status))
                r.status = status == 0 ? tr(kOffline) : tr("No se pudo descargar la lista de trucos. ¿Hay conexión a internet?");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        reply_ = std::move(r);
        hasReply_ = true;
    }
}

}  // namespace plumnin
