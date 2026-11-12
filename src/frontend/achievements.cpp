#include "achievements.h"

#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

#include "../core/i18n.h"
#include "../core/console.h"
#include "md5.h"
#include "rc_client.h"
#include "rc_consoles.h"

namespace plumnin {

namespace {

const char* const kPlumninAgent = "plumnin/0.0";

Achievements* owner(rc_client_t* client) { return static_cast<Achievements*>(rc_client_get_userdata(client)); }

std::string text(const char* s) { return s ? s : ""; }

}  // namespace

// A request to the server and, once made, its answer.
struct Achievements::Request {
    std::string url, post;
    void (*callback)(const rc_api_server_response_t*, void*) = nullptr;
    void* callbackData = nullptr;
    int status = 0;
    std::string body;
};

// What the worker threads share with poll(). It outlives the Achievements if a request
// is still being made when plumnin closes.
struct Achievements::Shared {
    std::mutex mutex;
    std::vector<std::unique_ptr<Request>> done;
};

Achievements::Achievements(Http http, bool threaded)
    : http_(std::move(http)), threaded_(threaded), shared_(std::make_shared<Shared>()) {
    client_ = rc_client_create(&Achievements::readMemoryCallback, &Achievements::serverCall);
    rc_client_set_userdata(client_, this);
    rc_client_set_event_handler(client_, &Achievements::eventHandler);
    rc_client_enable_logging(client_, RC_CLIENT_LOG_LEVEL_WARN,
                             [](const char* message, const rc_client_t*) { std::fprintf(stderr, "logros: %s\n", message); });
    rc_client_set_hardcore_enabled(client_, 0);
}

Achievements::~Achievements() {
    // Answers still on their way find nobody to call: rc_client is gone.
    rc_client_destroy(client_);
}

std::string Achievements::userAgent() const {
    char clause[64];
    rc_client_get_user_agent_clause(client_, clause, sizeof clause);
    return std::string(kPlumninAgent) + " " + clause;
}

void Achievements::serverCall(const rc_api_request_t* request, void (*callback)(const rc_api_server_response_t*, void*),
                              void* callbackData, rc_client_t* client) {
    Achievements* self = owner(client);
    auto r = std::make_unique<Request>();
    r->url = text(request->url);
    r->post = text(request->post_data);
    r->callback = callback;
    r->callbackData = callbackData;
    if (!self->threaded_) {
        self->pending_.push_back(std::move(r));
        return;
    }
    std::thread([shared = self->shared_, http = self->http_, agent = self->userAgent(), r = std::move(r)]() mutable {
        r->status = http(r->url, r->post, agent, r->body);
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->done.push_back(std::move(r));
    }).detach();
}

void Achievements::poll() {
    if (!threaded_) {
        // Requests made while answering others wait for the next poll().
        std::vector<std::unique_ptr<Request>> now = std::move(pending_);
        pending_.clear();
        for (auto& r : now) {
            r->status = http_(r->url, r->post, userAgent(), r->body);
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->done.push_back(std::move(r));
        }
    }
    std::vector<std::unique_ptr<Request>> done;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        done.swap(shared_->done);
    }
    for (auto& r : done) {
        rc_api_server_response_t response{};
        response.body = r->body.c_str();
        response.body_length = r->body.size();
        // Not reached at all: rcheevos tries again later.
        response.http_status_code = r->status > 0 ? r->status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        r->callback(&response, r->callbackData);
    }
}

// Account.

void Achievements::loginWithPassword(const std::string& user, const std::string& password) {
    loggingIn_ = true;
    loginError_.clear();
    rc_client_begin_login_with_password(client_, user.c_str(), password.c_str(), &Achievements::loginDone, nullptr);
}

void Achievements::loginWithToken(const std::string& user, const std::string& token) {
    loggingIn_ = true;
    loginError_.clear();
    rc_client_begin_login_with_token(client_, user.c_str(), token.c_str(), &Achievements::loginDone, nullptr);
}

void Achievements::loginDone(int result, const char* error, rc_client_t* client, void*) {
    Achievements* self = owner(client);
    self->loggingIn_ = false;
    if (result != RC_OK) {
        self->loginError_ = result == RC_INVALID_CREDENTIALS ? std::string(tr("Usuario o contraseña incorrectos."))
                            : result == RC_EXPIRED_TOKEN  ? std::string(tr("La sesión ha caducado: vuelve a escribir la contraseña."))
                            : result == RC_NO_RESPONSE    ? std::string(tr("No se puede conectar con RetroAchievements."))
                                                          : trf("No se pudo iniciar sesión: %s", text(error).c_str());
        self->notice(Notice::Kind::Info, "RetroAchievements", self->loginError_, {}, 0, true);
        return;
    }
    self->notice(Notice::Kind::Info, "RetroAchievements",
                 trf("Sesión iniciada como %s (%u puntos)", self->userName().c_str(), self->score()));
    if (self->wantGame_) self->beginLoadGame();
}

void Achievements::logout() {
    unloadGame();
    rc_client_logout(client_);
    loggingIn_ = false;
    loginError_.clear();
}

bool Achievements::loggedIn() const { return rc_client_get_user_info(client_) != nullptr; }

std::string Achievements::userName() const {
    const rc_client_user_t* u = rc_client_get_user_info(client_);
    return u ? text(u->display_name) : std::string();
}

std::string Achievements::token() const {
    const rc_client_user_t* u = rc_client_get_user_info(client_);
    return u ? text(u->token) : std::string();
}

uint32_t Achievements::score() const {
    const rc_client_user_t* u = rc_client_get_user_info(client_);
    return u ? u->score : 0;
}

void Achievements::setHardcore(bool on) { rc_client_set_hardcore_enabled(client_, on ? 1 : 0); }

bool Achievements::hardcore() const { return rc_client_get_hardcore_enabled(client_) != 0; }

// The game.

void Achievements::loadGame(Console& console, const std::string& hash) {
    unloadGame();
    console_ = &console;
    hash_ = hash;
    wantGame_ = true;
    if (loggedIn()) beginLoadGame();  // else once the login is done
}

void Achievements::beginLoadGame() {
    if (!console_ || hash_.empty()) return;
    loadingGame_ = true;
    rc_client_begin_load_game(client_, hash_.c_str(), &Achievements::gameDone, nullptr);
}

void Achievements::gameDone(int result, const char* error, rc_client_t* client, void*) {
    Achievements* self = owner(client);
    self->loadingGame_ = false;
    if (result == RC_ABORTED) return;  // another game was loaded meanwhile
    if (result != RC_OK) {
        self->notice(Notice::Kind::Info, "RetroAchievements",
                     result == RC_NO_GAME_LOADED ? std::string(tr("RetroAchievements no conoce este juego (o esta versión de la ROM)."))
                                                 : trf("No se pudieron cargar los logros: %s", text(error).c_str()),
                     {}, 0, result != RC_NO_GAME_LOADED);
        return;
    }
    const rc_client_game_t* game = rc_client_get_game_info(client);
    if (!rc_client_has_achievements(client)) {
        self->notice(Notice::Kind::GameStarted, text(game->title), tr("Este juego no tiene logros todavía."),
                     text(game->badge_url));
        return;
    }
    rc_client_user_game_summary_t s{};
    rc_client_get_user_game_summary(client, &s);
    std::string line = trf("Tienes %u de %u logros (%u de %u puntos)", s.num_unlocked_achievements,
                           s.num_core_achievements, s.points_unlocked, s.points_core);
    if (self->hardcore()) line += tr(". Modo hardcore");
    self->notice(Notice::Kind::GameStarted, text(game->title), line, text(game->badge_url));
}

void Achievements::unloadGame() {
    wantGame_ = false;
    loadingGame_ = false;
    console_ = nullptr;
    hash_.clear();
    challenges_.clear();
    trackers_.clear();
    progressShown_ = false;
    // Events still pending (the game completed by the last achievement, sent but not yet
    // shown) are raised by the unload after it has let go of the game, and crash it;
    // resetting raises them first.
    if (gameLoaded()) rc_client_reset(client_);
    rc_client_unload_game(client_);
}

bool Achievements::gameLoaded() const { return rc_client_is_game_loaded(client_) != 0; }

void Achievements::reset() { rc_client_reset(client_); }

void Achievements::doFrame() {
    if (console_) rc_client_do_frame(client_);
    else rc_client_idle(client_);
}

void Achievements::idle() { rc_client_idle(client_); }

std::vector<uint8_t> Achievements::saveProgress() {
    std::vector<uint8_t> data(gameLoaded() ? rc_client_progress_size(client_) : 0);
    if (!data.empty() && rc_client_serialize_progress_sized(client_, data.data(), data.size()) != RC_OK) data.clear();
    return data;
}

void Achievements::loadProgress(const std::vector<uint8_t>& data) {
    if (!gameLoaded()) return;
    if (data.empty()) rc_client_deserialize_progress_sized(client_, nullptr, 0);
    else rc_client_deserialize_progress_sized(client_, data.data(), data.size());
    // The challenges are not cleared here: rcheevos hides the ones the state does not
    // have and keeps the ones it has without showing them again, so clearing them would
    // lose those.
}

// Identifying the game and reading its memory.

std::string Achievements::gameHash(Console& console) {
    const std::vector<uint8_t>& rom = console.rom();
    if (rom.empty()) return {};
    // What RetroAchievements hashes: the cartridge without the headers that dumps add,
    // the 16 bytes of the NES's iNES header and the 512 of a SNES copier's.
    size_t skip = 0;
    if (console.system() == ConsoleSystem::Nes && rom.size() > 16 && rom[0] == 'N' && rom[1] == 'E' && rom[2] == 'S' &&
        rom[3] == 0x1A)
        skip = 16;
    if (console.system() == ConsoleSystem::Snes && rom.size() % 1024 == 512) skip = 512;
    md5_state_t md5;
    md5_init(&md5);
    md5_append(&md5, rom.data() + skip, static_cast<int>(rom.size() - skip));
    md5_byte_t digest[16];
    md5_finish(&md5, digest);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 32);
}

uint32_t Achievements::readMemory(Console& console, uint32_t address, uint8_t* buffer, uint32_t count) {
    // The console lays its memory out in RetroAchievements' order; the gaps read as nothing.
    const std::vector<MemoryRegion> map = console.memoryMap();
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t a = address + i;
        bool found = false;
        for (const MemoryRegion& m : map)
            if (a >= m.address && a - m.address < m.size) {
                buffer[i] = m.data[a - m.address];
                found = true;
                break;
            }
        if (!found) return i;
    }
    return count;
}

uint32_t Achievements::readMemoryCallback(uint32_t address, uint8_t* buffer, uint32_t count, rc_client_t* client) {
    Achievements* self = owner(client);
    return self->console_ ? readMemory(*self->console_, address, buffer, count) : 0;
}

// What happens while playing.

void Achievements::notice(Notice::Kind kind, std::string title, std::string text, std::string badge, uint32_t points,
                          bool error) {
    Notice n;
    n.kind = kind;
    n.title = std::move(title);
    n.text = std::move(text);
    n.badgeUrl = std::move(badge);
    n.points = points;
    n.error = error;
    notices_.push_back(std::move(n));
}

std::vector<Achievements::Notice> Achievements::takeNotices() {
    std::vector<Notice> out;
    out.swap(notices_);
    return out;
}

void Achievements::eventHandler(const rc_client_event_t* event, rc_client_t* client) {
    Achievements* self = owner(client);
    const rc_client_achievement_t* a = event->achievement;
    const rc_client_leaderboard_t* lb = event->leaderboard;
    auto removeIndicator = [](std::vector<Indicator>& list, uint32_t id) {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == id) {
                list.erase(list.begin() + static_cast<long>(i));
                return;
            }
    };
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            self->notice(Notice::Kind::Unlocked, text(a->title), text(a->description), text(a->badge_url), a->points);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
            removeIndicator(self->challenges_, a->id);
            self->challenges_.push_back({a->id, text(a->badge_url), text(a->title)});
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
            removeIndicator(self->challenges_, a->id);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
            self->progress_ = {a->id, text(a->badge_url), text(a->measured_progress)};
            self->progressShown_ = true;
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
            self->progressShown_ = false;
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
            self->notice(Notice::Kind::Leaderboard, trf("Reto empezado: %s", text(lb->title).c_str()), text(lb->description));
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
            self->notice(Notice::Kind::Leaderboard, trf("Reto fallido: %s", text(lb->title).c_str()));
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
            self->notice(Notice::Kind::Leaderboard, trf("Marca enviada: %s", text(lb->title).c_str()), text(lb->tracker_value));
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD: {
            const rc_client_leaderboard_scoreboard_t* s = event->leaderboard_scoreboard;
            self->notice(Notice::Kind::Leaderboard, trf("Tu puesto: %u de %u", s->new_rank, s->num_entries),
                         trf("Tu mejor marca: %s", text(s->best_score).c_str()));
            break;
        }
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
            removeIndicator(self->trackers_, event->leaderboard_tracker->id);
            self->trackers_.push_back({event->leaderboard_tracker->id, {}, text(event->leaderboard_tracker->display)});
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
            removeIndicator(self->trackers_, event->leaderboard_tracker->id);
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
            for (Indicator& t : self->trackers_)
                if (t.id == event->leaderboard_tracker->id) t.text = text(event->leaderboard_tracker->display);
            break;
        case RC_CLIENT_EVENT_RESET:
            self->notice(Notice::Kind::Reset, tr("Modo hardcore"), tr("El juego vuelve a empezar: así se juega en hardcore."));
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED: {
            const rc_client_game_t* game = rc_client_get_game_info(client);
            self->notice(Notice::Kind::GameCompleted, game ? text(game->title) : std::string(),
                         self->hardcore() ? tr("¡Juego dominado! Tienes todos sus logros en hardcore.")
                                          : tr("¡Juego completado! Tienes todos sus logros."),
                         game ? text(game->badge_url) : std::string());
            break;
        }
        case RC_CLIENT_EVENT_SERVER_ERROR:
            self->notice(Notice::Kind::Info, "RetroAchievements", trf("Error del servidor: %s", text(event->server_error->error_message).c_str()),
                         {}, 0, true);
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            self->notice(Notice::Kind::Info, "RetroAchievements",
                         tr("Sin conexión: los logros se enviarán cuando vuelva."), {}, 0, true);
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            self->notice(Notice::Kind::Info, "RetroAchievements", tr("Conexión recuperada: logros enviados."));
            break;
        default: break;
    }
}

// The list and the summary.

std::vector<Achievements::Group> Achievements::achievementList() {
    std::vector<Group> out;
    if (!gameLoaded()) return out;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client_, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) return out;
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const rc_client_achievement_bucket_t& bucket = list->buckets[b];
        Group g;
        switch (bucket.bucket_type) {
            case RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED: g.label = tr("Por conseguir"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED: g.label = tr("Conseguidos"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: g.label = tr("No se pueden conseguir en plumnin"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL: g.label = tr("No oficiales"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED: g.label = tr("Recién conseguidos"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE: g.label = tr("A punto (reto en marcha)"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE: g.label = tr("Casi"); break;
            case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED: g.label = tr("Conseguidos, sin enviar todavía"); break;
            default: g.label = text(bucket.label); break;
        }
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            Item it;
            it.title = text(a->title);
            it.description = text(a->description);
            it.unlocked = a->unlocked != 0;
            it.badgeUrl = text(it.unlocked ? a->badge_url : a->badge_locked_url);
            it.points = a->points;
            it.progress = a->measured_progress;
            it.percent = a->measured_percent;
            g.items.push_back(std::move(it));
        }
        out.push_back(std::move(g));
    }
    rc_client_destroy_achievement_list(list);
    return out;
}

std::string Achievements::gameTitle() const {
    const rc_client_game_t* g = gameLoaded() ? rc_client_get_game_info(client_) : nullptr;
    return g ? text(g->title) : std::string();
}

std::string Achievements::gameBadgeUrl() const {
    const rc_client_game_t* g = gameLoaded() ? rc_client_get_game_info(client_) : nullptr;
    return g ? text(g->badge_url) : std::string();
}

namespace {
rc_client_user_game_summary_t summary(const rc_client_t* client) {
    rc_client_user_game_summary_t s{};
    if (rc_client_is_game_loaded(const_cast<rc_client_t*>(client))) rc_client_get_user_game_summary(client, &s);
    return s;
}
}  // namespace

uint32_t Achievements::achievementCount() const { return summary(client_).num_core_achievements; }
uint32_t Achievements::unlockedCount() const { return summary(client_).num_unlocked_achievements; }
uint32_t Achievements::pointsTotal() const { return summary(client_).points_core; }
uint32_t Achievements::pointsUnlocked() const { return summary(client_).points_unlocked; }

std::string Achievements::richPresence() {
    char buffer[256];
    if (!gameLoaded() || !rc_client_has_rich_presence(client_)) return {};
    rc_client_get_rich_presence_message(client_, buffer, sizeof buffer);
    return buffer;
}

}  // namespace plumnin
