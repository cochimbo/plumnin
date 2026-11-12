// The library's "Medios de ScreenScraper" window: what was downloaded of a game, a
// search to pick the right game when it is the wrong one, the kinds of media to
// download and the account.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <system_error>

#include "imgui.h"
#include "theme.h"
#include "ui.h"
#include "../core/i18n.h"
#include "../core/system.h"

namespace plumnin {

namespace {

ScreenScraper::Rom romOf(const Library::Game& g) { return {g.path, g.stem, g.title, g.tags, g.system}; }

void openFile(const std::string& path) { openInSystem(path); }

// An image fitted into a w x h box, centred.
void fitImage(unsigned int texture, int iw, int ih, float w, float h) {
    float dw = w, dh = w * static_cast<float>(ih) / static_cast<float>(iw);
    if (dh > h) {
        dh = h;
        dw = h * static_cast<float>(iw) / static_cast<float>(ih);
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 a(at.x + (w - dw) * 0.5f, at.y + (h - dh) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture), a, ImVec2(a.x + dw, a.y + dh));
}

void copy(char* to, size_t size, const std::string& from) { std::snprintf(to, size, "%s", from.c_str()); }

}  // namespace

void Ui::applyScraperSettings() {
    SsCredentials c = ScreenScraper::builtIn();
    // One in plumnin.ini (typed in by an older version) only for a build without it.
    if (!c.usable() && !settings_.ssDevId.empty()) {
        c.devId = settings_.ssDevId;
        c.devPassword = settings_.ssDevPassword;
    }
    c.user = settings_.ssUser;
    c.password = settings_.ssPassword;
    scraper_.setCredentials(c);
    std::vector<std::string> kinds;
    std::stringstream list(settings_.ssMedia);
    for (std::string k; std::getline(list, k, ',');)
        if (ssKind(k)) kinds.push_back(k);
    if (kinds.empty() && !settings_.ssMedia.empty()) kinds.push_back("ninguno");  // none chosen, not the usual ones
    scraper_.setKinds(kinds);
    scraper_.setOverwrite(settings_.ssOverwrite);
    scraper_.setLanguage(language());
}

void Ui::openMedia(size_t game) {
    const auto& games = library_.games();
    if (game >= games.size()) return;
    const Library::Game& g = games[game];
    applyScraperSettings();
    scraper_.close();
    scraper_.show(romOf(g));
    media_.path = g.path;
    copy(media_.query, sizeof media_.query, g.title);
    media_.system = static_cast<int>(g.system);
    media_.selected = -1;
    media_.tab = 0;
    media_.selectTab = true;
    copy(media_.user, sizeof media_.user, settings_.ssUser);
    copy(media_.password, sizeof media_.password, settings_.ssPassword);
    showMedia_ = true;
}

void Ui::startMedia(size_t game, int mode) {
    openMedia(game);
    if (!showMedia_) return;
    const Library::Game& g = library_.games()[game];
    if (mode == 3) {
        std::vector<ScreenScraper::Rom> roms;
        for (const auto& other : library_.games()) roms.push_back(romOf(other));
        scraper_.downloadAll(roms, settings_.ssOverwrite);
    } else if (mode == 2) {
        media_.tab = 1;
        media_.autoPick = true;
        scraper_.search(media_.query, g.system);
    } else {
        scraper_.download(romOf(g));
    }
}

void Ui::scrapeLibrary(int overwrite) {
    applyScraperSettings();
    if (!scraper_.credentials().usable()) {
        openScraping(0);  // nothing can be asked without the developer account
        return;
    }
    std::vector<ScreenScraper::Rom> roms;
    for (const auto& g : library_.games()) roms.push_back(romOf(g));
    scraper_.downloadAll(roms, overwrite < 0 ? settings_.ssOverwrite : overwrite == 1);
}

void Ui::openScraping(int tab) {
    applyScraperSettings();
    copy(media_.user, sizeof media_.user, settings_.ssUser);
    copy(media_.password, sizeof media_.password, settings_.ssPassword);
    scrapingTab_ = tab;
    showScraping_ = true;
}

// Scraping > ...: ScreenScraper's account and what to download, for every game.
void Ui::drawScraping(UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(theme::px(640.0f), theme::px(560.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin((std::string(tr("Scraping")) + "###scraping").c_str(), &showScraping_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("pestanas_scraping")) {
        const char* tabs[] = {N_("Cuenta"), N_("Qué descargar")};
        for (int t = 0; t < 2; ++t) {
            const ImGuiTabItemFlags f = scrapingTab_ == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem((std::string(tr(tabs[t])) + "###s" + std::to_string(t)).c_str(), nullptr, f)) continue;
            if (t == 0) drawMediaAccount(out);
            else drawMediaKinds(out);
            ImGui::EndTabItem();
        }
        scrapingTab_ = -1;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Ui::drawMedia(UiActions& out) {
    auto& games = library_.games();
    const Library::Game* game = nullptr;
    for (const auto& g : games)
        if (g.path == media_.path) game = &g;
    if (!game || !showLibrary_) showMedia_ = false;
    const ImVec2 view = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(theme::px(860.0f), view.x - 40), std::min(theme::px(620.0f), view.y - 40)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (showMedia_ && ImGui::Begin((std::string(tr("Medios de ScreenScraper")) + "###medios").c_str(), &showMedia_,
                                   ImGuiWindowFlags_NoCollapse)) {
        const Library::Game& g = *game;
        const ScreenScraper::Sheet& sheet = scraper_.sheet();
        needGlyphs(g.title);
        ImGui::PushFont(theme::fonts().title);
        ImGui::TextUnformatted(g.title.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s", systemName(g.system), g.tags.empty() ? "" : " · ", g.tags.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.path.c_str());

        const bool usable = scraper_.credentials().usable();
        ImGui::BeginDisabled(sheet.busy || !usable);
        if (theme::primaryButton(sheet.known ? tr("Volver a descargar") : tr("Descargar de ScreenScraper"), 260.0f))
            scraper_.download(romOf(g), sheet.known ? sheet.game.id : 0);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", sheet.known ? tr("Descarga otra vez los medios del mismo juego de ScreenScraper.")
                                                : tr("Identifica el juego por su ROM (o por su nombre) y descarga sus medios."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Abrir la carpeta"))) {
            const std::string dir = scraper_.gameFolder(romOf(g));
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
            openFile(dir);
        }
        if (sheet.known && sheet.game.id) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Ver en screenscraper.fr")))
                SDL_OpenURL(("https://www.screenscraper.fr/gameinfos.php?gameid=" + std::to_string(sheet.game.id)).c_str());
        }
        if (!usable) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s",
                               tr("Esta compilación de plumnin no trae la cuenta de desarrollador de ScreenScraper."));
        } else if (sheet.busy) {
            ImGui::TextDisabled("%s", sheet.progress.empty() ? tr("Trabajando...") : sheet.progress.c_str());
        } else if (!sheet.status.empty()) {
            needGlyphs(sheet.status);
            ImGui::TextWrapped("%s", sheet.status.c_str());
        }
        // The library's round of downloads, if one is going or has stopped on an error.
        if (const std::string ss = scraper_.batchStatus(); !ss.empty() && (scraper_.batchLeft() > 0 || scraper_.batchFailed())) {
            if (scraper_.batchFailed()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.4f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", ss.c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::BeginTabBar("pestanas_medios")) {
            const char* tabs[] = {N_("Descargado"), N_("Buscar otro juego"), N_("Qué descargar"), N_("Cuenta")};
            for (int t = 0; t < 4; ++t) {
                const ImGuiTabItemFlags f = media_.selectTab && media_.tab == t ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem((std::string(tr(tabs[t])) + "###t" + std::to_string(t)).c_str(), nullptr, f)) continue;
                switch (t) {
                    case 0: drawMediaFound(g); break;
                    case 1: drawMediaSearch(g); break;
                    case 2: drawMediaKinds(out); break;
                    case 3: drawMediaAccount(out); break;
                }
                ImGui::EndTabItem();
            }
            media_.selectTab = false;
            ImGui::EndTabBar();
        }
        ImGui::End();
    } else if (showMedia_) {
        ImGui::End();
    }
    if (!showMedia_) scraper_.close();
}

// The files downloaded, as thumbnails (the cursor over one shows it bigger, a click
// opens it), then ScreenScraper's data of the game.
void Ui::drawMediaFound(const Library::Game& g) {
    const ScreenScraper::Sheet& sheet = scraper_.sheet();
    ImGui::BeginChild("descargado", ImVec2(0, 0));
    if (!sheet.known && sheet.media.empty()) {
        ImGui::TextDisabled("%s", sheet.busy ? "..." : tr("Aún no se ha descargado nada de este juego: pulsa Descargar de ScreenScraper."));
        ImGui::EndChild();
        return;
    }
    const std::vector<std::string> regions = ssRegions(g.tags), languages = ssLanguages(language());
    if (sheet.known) {
        const SsGame& s = sheet.game;
        const std::string name = s.name(regions);
        needGlyphs(name);
        ImGui::Text("%s", trf("En ScreenScraper: «%s»", name.c_str()).c_str());
        const std::string genre = s.genre(languages);
        const std::pair<const char*, std::string> rows[] = {
            {N_("Fecha"), s.date(regions)},  {N_("Distribuidora"), s.publisher}, {N_("Desarrolladora"), s.developer},
            {N_("Género"), genre},           {N_("Jugadores"), s.players},
            {N_("Nota"), s.rating.empty() ? std::string() : s.rating + "/20"},
        };
        if (ImGui::BeginTable("datos_ss", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##campo", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##valor", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [label, value] : rows) {
                if (value.empty()) continue;
                needGlyphs(value);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", tr(label));
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
            }
            ImGui::EndTable();
        }
    }

    // The thumbnails, as many to a row as fit.
    ImGui::SeparatorText(tr("Archivos"));
    const float cell = 150.0f, imageH = 150.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (cell + style.ItemSpacing.x)));
    int col = 0;
    for (const ScreenScraper::Medium& m : sheet.media) {
        const SsKind* k = ssKind(m.kind);
        if (!k) continue;
        if (col++ % cols) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(m.kind.c_str());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + cell, at.y + imageH), IM_COL32(40, 44, 56, 255), 4.0f);
        if (m.texture && m.width > 0 && m.height > 0) {
            fitImage(m.texture, m.width, m.height, cell, imageH);
        } else {
            const char* what = k->image ? "?" : m.kind == "video" ? tr("Vídeo") : tr("PDF");
            const ImVec2 size = ImGui::CalcTextSize(what);
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + (cell - size.x) * 0.5f, at.y + (imageH - size.y) * 0.5f),
                                                IM_COL32(170, 176, 190, 255), what);
            ImGui::Dummy(ImVec2(cell, imageH));
        }
        if (ImGui::IsItemClicked()) openFile(m.file);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (m.texture && m.width > 0) fitImage(m.texture, m.width, m.height, 420.0f, 420.0f);
            ImGui::TextDisabled("%s", m.file.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
        ImGui::TextUnformatted(tr(k->label));
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        ImGui::EndGroup();
    }

    // What ScreenScraper has that was not downloaded (videos and manuals are not, unless chosen).
    if (sheet.known) {
        bool any = false;
        for (const SsKind& k : ssKinds()) {
            if (!ssPickMedia(sheet.game, k, regions)) continue;
            if (std::any_of(sheet.media.begin(), sheet.media.end(), [&](const ScreenScraper::Medium& m) { return m.kind == k.id; }))
                continue;
            if (!any) {
                ImGui::SeparatorText(tr("También tiene"));
                any = true;
            } else {
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(sheet.busy);
            if (ImGui::Button(trf("Descargar: %s", tr(k.label)).c_str())) scraper_.download(romOf(g), sheet.game.id, {k.id});
            ImGui::EndDisabled();
        }
    }

    if (sheet.known) {
        const std::string text = sheet.game.text(languages);
        if (!text.empty()) {
            ImGui::SeparatorText(tr("Descripción"));
            needGlyphs(text);
            ImGui::TextWrapped("%s", text.c_str());
        }
    }
    ImGui::EndChild();
}

// Looking for the game by name, in any of the consoles: the results, with the cover of
// the one picked, and using it.
void Ui::drawMediaSearch(const Library::Game& g) {
    const ScreenScraper::Search& search = scraper_.searchState();
    const std::vector<std::string> regions = ssRegions(g.tags), languages = ssLanguages(language());
    ImGui::SetNextItemWidth(-260.0f);
    bool go = ImGui::InputTextWithHint("##buscar", tr("Nombre del juego"), media_.query, sizeof media_.query,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##consola", systemName(static_cast<ConsoleSystem>(media_.system)))) {
        for (int s = 0; s < kSystemCount; ++s)
            if (ImGui::Selectable(systemName(static_cast<ConsoleSystem>(s)), s == media_.system)) media_.system = s;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!media_.query[0] || !scraper_.credentials().usable());
    go |= ImGui::Button(tr("Buscar"), ImVec2(-1, 0));
    ImGui::EndDisabled();
    if (go && media_.query[0] && scraper_.credentials().usable()) {
        media_.selected = -1;
        media_.autoPick = true;
        scraper_.search(media_.query, static_cast<ConsoleSystem>(media_.system));
    }
    if (media_.autoPick && !search.busy && !scraper_.sheet().busy) {
        media_.autoPick = false;
        for (size_t i = 0; i < search.results.size() && media_.selected < 0; ++i)
            if (search.results[i].id == scraper_.sheet().game.id) media_.selected = static_cast<int>(i);
        if (media_.selected < 0 && !search.results.empty()) media_.selected = 0;
        if (media_.selected >= 0) scraper_.preview(search.results[static_cast<size_t>(media_.selected)], regions);
    }
    if (search.busy) ImGui::TextDisabled("%s", tr("Buscando..."));
    else if (!search.status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s", search.status.c_str());
    else if (!search.results.empty())
        ImGui::TextDisabled(tr("%d resultados. Pulsa uno para verlo."), static_cast<int>(search.results.size()));
    else ImGui::TextDisabled("%s", tr("Cambia el nombre si no sale el juego: sin región ni número de versión suele ir mejor."));

    const float side = std::min(260.0f, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("resultados", ImVec2(-side - ImGui::GetStyle().ItemSpacing.x, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(search.results.size()); ++i) {
        const SsGame& r = search.results[static_cast<size_t>(i)];
        std::string line = r.name(regions);
        const std::string year = r.year(regions);
        if (!year.empty()) line += " · " + year;
        if (!r.publisher.empty()) line += " · " + r.publisher;
        if (!r.systemName.empty()) line += " · " + r.systemName;
        needGlyphs(line);
        ImGui::PushID(i);
        if (ImGui::Selectable(line.c_str(), media_.selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            media_.selected = i;
            scraper_.preview(r, regions);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                scraper_.download(romOf(g), r.id);
                media_.tab = 0;
                media_.selectTab = true;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const bool picked = media_.selected >= 0 && media_.selected < static_cast<int>(search.results.size());
    const float boxH = std::min(320.0f, ImGui::GetContentRegionAvail().y * 0.6f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + side, at.y + boxH), IM_COL32(40, 44, 56, 255), 4.0f);
    if (picked && search.preview && search.previewId == search.results[static_cast<size_t>(media_.selected)].id && search.height > 0) {
        fitImage(search.preview, search.width, search.height, side, boxH);
    } else {
        const char* what = picked && search.previewBusy ? "..." : picked ? tr("Sin imagen") : "";
        const ImVec2 size = ImGui::CalcTextSize(what);
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + (side - size.x) * 0.5f, at.y + (boxH - size.y) * 0.5f),
                                            IM_COL32(170, 176, 190, 255), what);
        ImGui::Dummy(ImVec2(side, boxH));
    }
    ImGui::BeginDisabled(!picked || scraper_.sheet().busy);
    if (theme::primaryButton(tr("Usar este juego"), side) && picked) {
        scraper_.download(romOf(g), search.results[static_cast<size_t>(media_.selected)].id);
        media_.tab = 0;
        media_.selectTab = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Descarga los medios de este juego en lugar de los que había, y su portada y etiqueta\n"
                                   "pasan a ser las de la biblioteca."));
    if (picked) {
        const SsGame& r = search.results[static_cast<size_t>(media_.selected)];
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
        if (!r.developer.empty()) ImGui::TextDisabled("%s", trf("Desarrolladora: %s", r.developer.c_str()).c_str());
        if (!r.players.empty()) ImGui::TextDisabled("%s", trf("Jugadores: %s", r.players.c_str()).c_str());
        const std::string genre = r.genre(languages);
        if (!genre.empty()) ImGui::TextDisabled("%s", genre.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
}

void Ui::drawMediaKinds(UiActions& out) {
    // What a game that has already been downloaded gets the next time.
    ImGui::SeparatorText(tr("Al volver a descargar"));
    int mode = settings_.ssOverwrite ? 1 : 0;
    bool modeChanged = ImGui::RadioButton(tr("Añadir solo lo que falte (rápido: no vuelve a pedir el juego)"), &mode, 0);
    modeChanged |= ImGui::RadioButton(tr("Sobrescribir todos los medios"), &mode, 1);
    if (modeChanged) {
        settings_.ssOverwrite = mode == 1;
        applyScraperSettings();
        out.settingsChanged = true;
    }
    ImGui::SeparatorText(tr("Qué descargar"));
    ImGui::TextWrapped("%s", tr("Lo que se descarga de cada juego. Los vídeos y los manuales ocupan mucho, así que no se "
                                "descargan si no los marcas (cada juego los puede descargar aparte)."));
    std::vector<std::string> chosen;
    std::stringstream list(settings_.ssMedia);
    for (std::string k; std::getline(list, k, ',');)
        if (ssKind(k)) chosen.push_back(k);
    if (settings_.ssMedia.empty())
        for (const SsKind& k : ssKinds())
            if (k.byDefault) chosen.push_back(k.id);
    bool changed = false;
    for (const SsKind& k : ssKinds()) {
        bool on = std::find(chosen.begin(), chosen.end(), k.id) != chosen.end();
        if (!ImGui::Checkbox(tr(k.label), &on)) continue;
        changed = true;
        if (on) chosen.push_back(k.id);
        else chosen.erase(std::remove(chosen.begin(), chosen.end(), k.id), chosen.end());
    }
    if (!changed) return;
    // In the window's order, so the file reads the same way.
    std::string value;
    for (const SsKind& k : ssKinds())
        if (std::find(chosen.begin(), chosen.end(), k.id) != chosen.end()) value += (value.empty() ? "" : ",") + std::string(k.id);
    settings_.ssMedia = value.empty() ? "ninguno" : value;  // nothing chosen is not "the usual ones"
    applyScraperSettings();
    out.settingsChanged = true;
}

void Ui::drawMediaAccount(UiActions& out) {
    // The user's own account: optional, for more requests a day.
    ImGui::SeparatorText(tr("Tu cuenta de screenscraper.fr"));
    ImGui::TextWrapped("%s", tr("Si ya tienes cuenta, escribe aquí tu usuario y tu contraseña de screenscraper.fr: con ella "
                                "se pueden hacer más peticiones al día, y ScreenScraper sigue respondiendo aunque esté lleno. "
                                "Se guarda en plumnin.ini."));
    ImGui::Spacing();
    const float labelW = std::max(ImGui::CalcTextSize(tr("Usuario")).x, ImGui::CalcTextSize(tr("Contraseña")).x) +
                         ImGui::GetStyle().ItemSpacing.x * 2;
    auto field = [&](const char* label, const char* id, char* buf, size_t size, ImGuiInputTextFlags flags) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(theme::px(260.0f));
        ImGui::InputText(id, buf, size, flags);
        return ImGui::IsItemDeactivatedAfterEdit();
    };
    // Kept when a field is left (or with the button), not on every key.
    bool changed = field(tr("Usuario"), "##ss_usuario", media_.user, sizeof media_.user, 0);
    changed |= field(tr("Contraseña"), "##ss_contrasena", media_.password, sizeof media_.password, ImGuiInputTextFlags_Password);
    ImGui::SetCursorPosX(labelW);
    changed |= ImGui::Button(tr("Usar esta cuenta"));
    if (!settings_.ssUser.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Quitar la cuenta"))) {
            media_.user[0] = media_.password[0] = '\0';
            changed = true;
        }
    }
    ImGui::SetCursorPosX(labelW);
    if (settings_.ssUser.empty()) ImGui::TextDisabled("%s", tr("Ahora no se usa ninguna cuenta tuya."));
    else ImGui::TextDisabled("%s", trf("Se usa la cuenta «%s».", settings_.ssUser.c_str()).c_str());
    // Whether ScreenScraper takes the accounts, without downloading anything.
    ImGui::SetCursorPosX(labelW);
    ImGui::BeginDisabled(scraper_.checking());
    if (ImGui::Button(tr("Probar la conexión"))) {
        applyScraperSettings();
        scraper_.check();
    }
    ImGui::EndDisabled();
    if (!scraper_.checkStatus().empty()) {
        ImGui::SameLine();
        const ImVec4 color = scraper_.checking() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                             : scraper_.checkOk() ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                                  : ImVec4(1.0f, 0.55f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", scraper_.checkStatus().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("¿No tienes cuenta? Es gratis:"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Crear una cuenta"))) SDL_OpenURL("https://www.screenscraper.fr/membreinscription.php");

    // The program's account is built into releases, as in other emulators: nothing to type.
    if (!ScreenScraper::builtIn().usable() && settings_.ssDevId.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", tr("Esta compilación de plumnin no trae la cuenta de desarrollador de ScreenScraper, así que no "
                                    "puede descargar nada. Los instaladores publicados sí la traen."));
        ImGui::PopStyleColor();
    }
    if (!changed) return;
    settings_.ssUser = media_.user;
    settings_.ssPassword = media_.password;
    applyScraperSettings();
    out.settingsChanged = true;
    scraper_.check();  // says right away whether ScreenScraper takes it
}

}  // namespace plumnin
