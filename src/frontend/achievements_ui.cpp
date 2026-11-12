#include "achievements_ui.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "theme.h"
#include "../core/i18n.h"
#include "http.h"
#include "../render/gl.h"

#define STBI_NO_STDIO
#include "stb_image.h"

namespace plumnin {

namespace {

constexpr Uint32 kUnlockMs = 5000;  // how long an achievement just earned stays on screen
constexpr Uint32 kNoticeMs = 4000;
constexpr size_t kPopupsShown = 3;
constexpr float kListBadge = 48.0f;
const ImVec4 kGold(1.0f, 0.82f, 0.25f, 1.0f);
const ImVec4 kWarning(1.0f, 0.6f, 0.3f, 1.0f);

}  // namespace

struct AchievementsUi::Badge {
    unsigned int texture = 0;
};

// The badges decoded by the worker threads, waiting to become textures.
struct AchievementsUi::Downloads {
    struct Image {
        std::string url;
        std::vector<unsigned char> rgba;
        int width = 0, height = 0;
    };
    std::mutex mutex;
    std::vector<Image> done;
};

AchievementsUi::AchievementsUi(Settings& settings, Achievements& achievements)
    : settings_(settings), ra_(achievements), downloads_(std::make_shared<Downloads>()) {
    std::snprintf(user_, sizeof user_, "%s", settings_.raUser.c_str());
}

AchievementsUi::~AchievementsUi() {
    for (auto& b : badges_)
        if (b.second->texture) gl::DeleteTextures(1, &b.second->texture);
}

unsigned int AchievementsUi::badge(const std::string& url) {
    if (url.empty()) return 0;
    auto it = badges_.find(url);
    if (it != badges_.end()) return it->second->texture;
    badges_[url] = std::make_unique<Badge>();
    std::thread([downloads = downloads_, url] {
        std::vector<uint8_t> data;
        Downloads::Image image;
        image.url = url;
        if (httpGet(url, data) == 200 && !data.empty()) {
            int n = 0;
            if (stbi_uc* rgba = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &image.width,
                                                      &image.height, &n, 4)) {
                image.rgba.assign(rgba, rgba + static_cast<size_t>(image.width) * image.height * 4);
                stbi_image_free(rgba);
            }
        }
        std::lock_guard<std::mutex> lock(downloads->mutex);
        downloads->done.push_back(std::move(image));
    }).detach();
    return 0;
}

void AchievementsUi::receiveBadges() {
    std::vector<Downloads::Image> done;
    {
        std::lock_guard<std::mutex> lock(downloads_->mutex);
        done.swap(downloads_->done);
    }
    for (const Downloads::Image& image : done) {
        auto it = badges_.find(image.url);
        if (it == badges_.end() || image.rgba.empty()) continue;  // a failed one stays blank
        unsigned int& texture = it->second->texture;
        gl::GenTextures(1, &texture);
        gl::BindTexture(gl::TEXTURE_2D, texture);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, image.width, image.height, 0, gl::RGBA, gl::UNSIGNED_BYTE,
                       image.rgba.data());
    }
}

void AchievementsUi::show(const Achievements::Notice& notice) {
    Popup p;
    p.notice = notice;
    p.until = 0;  // counted from when it is first drawn
    badge(notice.badgeUrl);  // start downloading it now
    popups_.push_back(std::move(p));
}

bool AchievementsUi::drawMenu(bool romLoaded) {
    bool changed = false;
    if (ImGui::MenuItem("RetroAchievements", nullptr, settings_.achievements)) {
        settings_.achievements = !settings_.achievements;
        changed = true;
        if (settings_.achievements && settings_.raToken.empty()) showAccount_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Logros de retroachievements.org: los mismos que en RetroArch y otros emuladores,\n"
                                   "con tu cuenta de allí."));
    if (ImGui::MenuItem(tr("Cuenta..."), nullptr, showAccount_)) showAccount_ = true;
    ImGui::BeginDisabled(!settings_.achievements || !ra_.gameLoaded());
    if (ImGui::MenuItem(tr("Logros de este juego..."), nullptr, showList_)) showList_ = true;
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Modo hardcore"), nullptr, settings_.raHardcore, settings_.achievements)) {
        settings_.raHardcore = !settings_.raHardcore;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tr("Los logros cuentan para tu puntuación hardcore y las clasificaciones, pero sin\n"
                             "ayudas: no se puede rebobinar, ir a cámara lenta, usar trucos ni cargar estados.%s"),
                          romLoaded ? tr("\nAl activarlo, el juego vuelve a empezar.") : "");
    ImGui::Separator();
    if (!settings_.achievements) ImGui::TextDisabled("%s", tr("Desactivados"));
    else if (ra_.loggedIn())
        ImGui::TextDisabled(tr("%s, %u puntos"), ra_.userName().c_str(), static_cast<unsigned>(ra_.score()));
    else if (ra_.loggingIn()) ImGui::TextDisabled("%s", tr("Iniciando sesión..."));
    else ImGui::TextDisabled("%s", tr("Sin sesión iniciada"));
    if (settings_.achievements && ra_.gameLoaded())
        ImGui::TextDisabled(tr("Este juego: %u de %u logros"), static_cast<unsigned>(ra_.unlockedCount()),
                            static_cast<unsigned>(ra_.achievementCount()));
    return changed;
}

bool AchievementsUi::drawWindows() {
    receiveBadges();
    bool changed = false;
    if (showAccount_) drawAccount(changed);
    if (showList_) drawList();
    return changed;
}

void AchievementsUi::drawAccount(bool& changed) {
    if (!ImGui::Begin("RetroAchievements", &showAccount_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextWrapped("%s", tr("Logros, marcas y retos para cada juego, los de retroachievements.org. Necesitas una "
                                "cuenta (gratis) allí."));
    if (ImGui::Checkbox(tr("Activar los logros"), &settings_.achievements)) changed = true;
    ImGui::BeginDisabled(!settings_.achievements);

    ImGui::SeparatorText(tr("Cuenta"));
    if (ra_.loggedIn()) {
        ImGui::Text(tr("Sesión iniciada como %s (%u puntos)."), ra_.userName().c_str(), static_cast<unsigned>(ra_.score()));
        if (ImGui::Button(tr("Cerrar sesión"))) {
            ra_.logout();
            settings_.raToken.clear();
            changed = true;
        }
    } else {
        ImGui::SetNextItemWidth(220);
        ImGui::InputText(tr("Usuario"), user_, sizeof user_);
        ImGui::SetNextItemWidth(220);
        const bool enter = ImGui::InputText(tr("Contraseña"), password_, sizeof password_,
                                            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(ra_.loggingIn() || !user_[0] || !password_[0]);
        if (ImGui::Button(ra_.loggingIn() ? tr("Iniciando sesión...") : tr("Iniciar sesión")) || (enter && user_[0] && password_[0])) {
            settings_.raUser = user_;
            ra_.loginWithPassword(user_, password_);
            std::fill(std::begin(password_), std::end(password_), '\0');  // only its token is kept
            changed = true;
        }
        ImGui::EndDisabled();
        if (!ra_.loginError().empty()) ImGui::TextColored(kWarning, "%s", ra_.loginError().c_str());
        ImGui::TextDisabled("%s", tr("La contraseña no se guarda: plumnin guarda la sesión que te da el servidor."));
        if (ImGui::SmallButton(tr("Crear una cuenta en retroachievements.org")))
            SDL_OpenURL("https://retroachievements.org/createaccount.php");
    }

    ImGui::SeparatorText(tr("Modo hardcore"));
    if (ImGui::Checkbox(tr("Jugar en hardcore"), &settings_.raHardcore)) changed = true;
    ImGui::TextDisabled("%s", tr("Sin rebobinar, cámara lenta, trucos ni cargar estados (guardar sí). Los logros cuentan "
                                 "para tu puntuación hardcore y se puede entrar en las clasificaciones. Al activarlo, el "
                                 "juego vuelve a empezar."));
    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void AchievementsUi::drawList() {
    ImGui::SetNextWindowSize(ImVec2(theme::px(520), theme::px(560)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Logros de este juego"), &showList_)) {
        ImGui::End();
        return;
    }
    if (!ra_.gameLoaded()) {
        ImGui::TextDisabled("%s", tr("No hay un juego con logros abierto."));
        ImGui::End();
        return;
    }
    if (unsigned int t = badge(ra_.gameBadgeUrl())) {
        ImGui::Image(static_cast<ImTextureID>(t), ImVec2(kListBadge, kListBadge));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextUnformatted(ra_.gameTitle().c_str());
    ImGui::Text(tr("%u de %u logros, %u de %u puntos%s"), static_cast<unsigned>(ra_.unlockedCount()),
                static_cast<unsigned>(ra_.achievementCount()), static_cast<unsigned>(ra_.pointsUnlocked()),
                static_cast<unsigned>(ra_.pointsTotal()), ra_.hardcore() ? tr(" (hardcore)") : "");
    const std::string presence = ra_.richPresence();
    if (!presence.empty()) ImGui::TextDisabled("%s", presence.c_str());
    ImGui::EndGroup();

    ImGui::BeginChild("lista");
    for (const Achievements::Group& g : ra_.achievementList()) {
        ImGui::SeparatorText(g.label.c_str());
        for (const Achievements::Item& a : g.items) {
            ImGui::PushID(&a);
            const unsigned int t = badge(a.badgeUrl);
            if (t) ImGui::Image(static_cast<ImTextureID>(t), ImVec2(kListBadge, kListBadge));
            else ImGui::Dummy(ImVec2(kListBadge, kListBadge));
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (a.unlocked) ImGui::TextColored(kGold, "%s", a.title.c_str());
            else ImGui::TextUnformatted(a.title.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%u puntos"), static_cast<unsigned>(a.points));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", a.description.c_str());
            ImGui::PopTextWrapPos();
            if (!a.progress.empty() && !a.unlocked)
                ImGui::ProgressBar(a.percent / 100.0f, ImVec2(200, 0), a.progress.c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void AchievementsUi::drawOverlay(float top, bool gameHidden) {
    receiveBadges();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    const float size = ImGui::GetFontSize();

    // Challenges, the achievement making progress and the leaderboards' counters: a row
    // in the top left corner.
    const auto& challenges = ra_.challenges();
    const Achievements::Indicator* progress = ra_.progress();
    const auto& trackers = ra_.trackers();
    if (!gameHidden && (!challenges.empty() || progress || !trackers.empty())) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 10, top + 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##logros_indicadores", nullptr, flags);
        bool first = true;
        for (const auto& c : challenges) {
            if (!first) ImGui::SameLine();
            first = false;
            if (unsigned int t = badge(c.badgeUrl)) ImGui::Image(static_cast<ImTextureID>(t), ImVec2(size * 2, size * 2));
            else ImGui::TextColored(kGold, "%s", c.text.c_str());
        }
        if (progress) {
            if (!first) ImGui::SameLine();
            first = false;
            if (unsigned int t = badge(progress->badgeUrl)) {
                ImGui::Image(static_cast<ImTextureID>(t), ImVec2(size * 2, size * 2));
                ImGui::SameLine();
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(progress->text.c_str());
        }
        for (const auto& tr : trackers) {
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kGold, "%s", tr.text.c_str());
        }
        ImGui::End();
    }

    // The notices, in the bottom right corner, the oldest at the bottom.
    const Uint32 now = SDL_GetTicks();
    while (!popups_.empty() && popups_.front().until && SDL_TICKS_PASSED(now, popups_.front().until)) popups_.pop_front();
    float y = vp->Pos.y + vp->Size.y - 10;
    for (size_t i = 0; i < popups_.size() && i < kPopupsShown; ++i) {
        Popup& p = popups_[i];
        const Achievements::Notice& n = p.notice;
        if (!p.until) p.until = now + (n.kind == Achievements::Notice::Kind::Unlocked ? kUnlockMs : kNoticeMs);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 10, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.8f);
        char id[32];
        std::snprintf(id, sizeof id, "##logro%zu", i);
        ImGui::Begin(id, nullptr, flags);
        if (unsigned int t = badge(n.badgeUrl)) {
            ImGui::Image(static_cast<ImTextureID>(t), ImVec2(size * 4, size * 4));
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size * 22);
        switch (n.kind) {
            case Achievements::Notice::Kind::Unlocked:
                ImGui::TextColored(kGold, tr("¡Logro desbloqueado! (%u puntos)"), static_cast<unsigned>(n.points));
                break;
            case Achievements::Notice::Kind::GameCompleted: ImGui::TextColored(kGold, "%s", tr("¡Enhorabuena!")); break;
            default: break;
        }
        if (n.error) ImGui::TextColored(kWarning, "%s", n.title.c_str());
        else ImGui::TextUnformatted(n.title.c_str());
        if (!n.text.empty()) ImGui::TextWrapped("%s", n.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        y -= ImGui::GetWindowHeight() + 6;
        ImGui::End();
    }
}

}  // namespace plumnin
