#include "instrument_editor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "imgui.h"
#include "file_dialog.h"
#include "../core/console.h"
#include "../core/i18n.h"
#include "../core/system.h"

namespace plumnin {

namespace {

std::string readText(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string noteName(int note) {
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    note = std::clamp(note, 0, 127);
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

const char* kindName(SynthMusic::Kind kind) {
    switch (kind) {
        case SynthMusic::Kind::Pulse: return tr("Pulso");
        case SynthMusic::Kind::Wave: return tr("Onda");
        case SynthMusic::Kind::Triangle: return tr("Triángulo");
        case SynthMusic::Kind::Noise: return tr("Ruido");
        case SynthMusic::Kind::Sample: return tr("Muestra");
    }
    return "";
}

// What the chip makes of it, in a few words.
std::string describe(const SynthMusic::Seen& s) {
    static const char* const kDuty[4] = {"12,5 %", "25 %", "50 %", "75 %"};
    const SynthMusic::Traits& t = s.traits;
    switch (s.kind) {
        case SynthMusic::Kind::Pulse: return trf("Pulso de %s", kDuty[t.duty & 3]);
        case SynthMusic::Kind::Wave: return tr("Onda de la Game Boy");
        case SynthMusic::Kind::Triangle: return tr("Triángulo de la NES");
        case SynthMusic::Kind::Noise: return trf("Ruido, altura %d de 15", t.noise);
        case SynthMusic::Kind::Sample:
            if (!t.pitched) return tr("Muestra sin nota");
            if (t.looped) return tr("Muestra con bucle");
            return trf("Muestra de %.2f s", t.seconds);
    }
    return {};
}

// What an assignment plays, for the drop-down.
std::string playsAs(const SynthMusic::Assignment& a) {
    if (a.original) return tr("Original (el chip)");
    if (a.drum >= 0) return trf("Batería: %s", SynthMusic::drumName(a.drum));
    if (a.program >= 0) return SynthMusic::programName(a.program);
    return tr("Nada");
}

}  // namespace

std::string InstrumentEditor::fileFor(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return folder + std::filesystem::u8path(game).stem().u8string() + ".ini";
}

SynthMusic::Assignments InstrumentEditor::load(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return SynthMusic::parseAssignments(readText(fileFor(folder, game)));
}

void InstrumentEditor::save(const SynthMusic& synth) {
    const std::string path = fileFor(folder_, game_);
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder_), ec);
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    out << SynthMusic::formatAssignments(synth.assignments());
    error_ = out ? std::string() : trf("No se pudo guardar %s", path.c_str());
}

void InstrumentEditor::assign(SynthMusic& synth, uint32_t fingerprint, const SynthMusic::Assignment* a) {
    SynthMusic::Assignments all = synth.assignments();
    if (a) all[fingerprint] = *a;
    else all.erase(fingerprint);
    synth.setAssignments(std::move(all));
    save(synth);
}

void InstrumentEditor::solo(SynthMusic& synth, uint32_t fingerprint) {
    solo_ = fingerprint;
    std::set<uint32_t> silenced;
    if (fingerprint)
        for (const auto& [fp, seen] : synth.seen())
            if (fp != fingerprint) silenced.insert(fp);
    synth.setListening(0, std::move(silenced));
}

void InstrumentEditor::draw(SynthMusic& synth, const std::string& game, bool on, Console* console, int chipPercent) {
    if (ripDone_) finishRip();
    if (recording_ && recording_ != console) {
        // The game was closed while recording: its console is gone, and so is the recording.
        recording_ = nullptr;
        message_ = tr("La grabación se ha perdido: el juego se cerró antes de pararla.");
    }
    if (game != game_) {
        game_ = game;
        error_.clear();
        if (solo_) solo(synth, 0);
        stopSong();
        scanned_ = false;
    }
    if (!open_) {
        if (solo_) solo(synth, 0);
        stopSong();
        return;
    }
    if (!scanned_) scanSongs();
    ImGui::SetNextWindowSize(ImVec2(900, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Editor de instrumentos"), &open_)) {
        ImGui::End();
        return;
    }
    if (game.empty()) {
        ImGui::TextWrapped("%s", tr("Carga un juego y juega: sus instrumentos aparecen aquí a medida que suenan."));
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("pestañas")) {
        if (ImGui::BeginTabItem(tr("Instrumentos"))) {
            drawInstruments(synth, on);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Canciones"))) {
            drawSongs(synth, on, console, chipPercent);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void InstrumentEditor::drawInstruments(SynthMusic& synth, bool on) {
    if (!on) {
        ImGui::TextWrapped("%s", tr("Elige Sonido > Sintetizador para que la música suene con el SoundFont; entonces sus "
                                    "instrumentos aparecen aquí a medida que suenan."));
        return;
    }
    ImGui::TextWrapped("%s", tr("Los instrumentos que el juego ha tocado hasta ahora, en el orden en que sonaron. Lo que "
                                "elijas se oye al momento y se guarda para este juego."));
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());

    std::vector<std::pair<uint32_t, SynthMusic::Seen>> list(synth.seen().begin(), synth.seen().end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.order < b.second.order; });
    // Which instrument each voice plays now, to light up those sounding.
    std::set<uint32_t> playing;
    const uint32_t active = synth.activeVoices();
    for (int v = 0; v < SynthMusic::kVoices; ++v)
        if ((active >> v) & 1) playing.insert(synth.voiceInstrument(v));

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable;
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    if (ImGui::BeginTable("instrumentos", 7, flags, ImVec2(0, -bottom))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Instrumento"), ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn(tr("En el chip"), ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn(tr("Canales"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(tr("Notas"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(tr("Suena como"));
        ImGui::TableSetupColumn(tr("Octava, volumen"), ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableHeadersRow();
        int letters[5] = {};
        for (const auto& [fp, seen] : list) {
            ImGui::PushID(static_cast<int>(fp));
            ImGui::TableNextRow();
            const bool assigned = synth.assignments().count(fp) != 0;
            const SynthMusic::Assignment now = synth.assignmentFor(fp);

            ImGui::TableNextColumn();
            const int letter = letters[static_cast<int>(seen.kind)]++;
            char name[64];
            std::snprintf(name, sizeof name, "%s %c", kindName(seen.kind), 'A' + letter % 26);
            if (playing.count(fp)) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s", name);
            else ImGui::TextUnformatted(name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %08x", tr("Huella"), fp);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(describe(seen).c_str());

            ImGui::TableNextColumn();
            std::string voices;
            for (int v = 0; v < SynthMusic::kVoices; ++v)
                if ((seen.voices >> v) & 1) voices += (voices.empty() ? "" : " ") + std::to_string(v + 1);
            ImGui::TextUnformatted(voices.c_str());

            ImGui::TableNextColumn();
            if (seen.kind == SynthMusic::Kind::Noise || (seen.kind == SynthMusic::Kind::Sample && !seen.traits.pitched))
                ImGui::TextDisabled("%d", seen.notes);
            else if (seen.low == seen.high)
                ImGui::Text("%s", noteName(seen.low).c_str());
            else
                ImGui::Text("%s-%s", noteName(seen.low).c_str(), noteName(seen.high).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("%d notas"), seen.notes);

            // What it plays: the rules' suggestion until chosen.
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            const std::string current = assigned ? playsAs(now) : trf("Sugerencia: %s", playsAs(seen.rules).c_str());
            if (ImGui::BeginCombo("##suena", current.c_str(), ImGuiComboFlags_HeightLarge)) {
                SynthMusic::Assignment a = now;
                auto pick = [&](SynthMusic::Assignment chosen) {
                    chosen.octave = now.octave;
                    chosen.volume = now.volume;
                    assign(synth, fp, &chosen);
                };
                if (ImGui::Selectable(tr("Original (el chip)"), now.original)) {
                    SynthMusic::Assignment o;
                    o.original = true;
                    pick(o);
                }
                ImGui::Separator();
                const bool noise = seen.kind == SynthMusic::Kind::Noise;
                if (!noise)
                    for (int p = 0; p < 128; ++p) {
                        char label[96];
                        std::snprintf(label, sizeof label, "%3d  %s", p + 1, SynthMusic::programName(p));
                        if (ImGui::Selectable(label, !now.original && now.drum < 0 && now.program == p)) {
                            SynthMusic::Assignment o;
                            o.program = p;
                            pick(o);
                        }
                    }
                ImGui::Separator();
                for (int d = 35; d <= 81; ++d) {
                    const std::string label = trf("Batería: %s", SynthMusic::drumName(d));
                    if (ImGui::Selectable(label.c_str(), !now.original && now.drum == d)) {
                        SynthMusic::Assignment o;
                        o.drum = d;
                        pick(o);
                    }
                }
                (void)a;
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(now.original);
            int octave = now.octave, volume = now.volume;
            ImGui::SetNextItemWidth(60);
            const bool drum = now.drum >= 0;
            ImGui::BeginDisabled(drum);
            if (ImGui::SliderInt("##octava", &octave, -3, 3, "%+d", ImGuiSliderFlags_AlwaysClamp) && octave != now.octave) {
                SynthMusic::Assignment o = now;
                o.octave = octave;
                assign(synth, fp, &o);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Octavas arriba o abajo"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::SliderInt("##volumen", &volume, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp) && volume != now.volume) {
                SynthMusic::Assignment o = now;
                o.volume = volume;
                assign(synth, fp, &o);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Volumen"));
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            const bool soloed = solo_ == fp;
            if (ImGui::SmallButton(soloed ? tr("Oír todos") : tr("Oír solo este"))) solo(synth, soloed ? 0 : fp);
            ImGui::SameLine();
            ImGui::BeginDisabled(!assigned);
            if (ImGui::SmallButton(tr("Sugerencia"))) assign(synth, fp, nullptr);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Vuelve a lo que dan las reglas"));
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (list.empty()) ImGui::TextDisabled("%s", tr("Todavía no ha sonado nada."));
    ImGui::BeginDisabled(synth.assignments().empty());
    if (ImGui::Button(tr("Todo a las sugerencias"))) {
        synth.setAssignments({});
        save(synth);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", fileFor(folder_, game_).c_str());
}

// ---------------------------------------------------------------------------------
// Canciones

InstrumentEditor::~InstrumentEditor() {
    if (ripThread_.joinable()) {
        ripProgress_->cancel = true;
        ripThread_.join();
    }
}

std::string InstrumentEditor::tracksFor(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return folder + "tracks/" + std::filesystem::u8path(game).stem().u8string() + "/";
}

void InstrumentEditor::scanSongs() {
    scanned_ = true;
    songs_.clear();
    const std::string dir = tracksFor(folder_, game_);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
        std::string ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".vgm" && ext != ".vgz") continue;
        Track t;
        t.path = entry.path().u8string();
        t.name = entry.path().stem().u8string();
        const std::string bytes = readText(t.path);
        VgmFile f;
        if (f.parse(std::vector<uint8_t>(bytes.begin(), bytes.end()), nullptr)) {
            t.seconds = f.seconds();
            t.loopSeconds = f.loopSamples / 44100.0;
            if (!f.title.empty()) t.name = f.title;
        }
        songs_.push_back(std::move(t));
    }
    std::sort(songs_.begin(), songs_.end(), [](const Track& a, const Track& b) { return a.path < b.path; });
}

void InstrumentEditor::startSong(size_t index, SynthMusic& synth, bool on, int chipPercent) {
    stopSong();
    if (index >= songs_.size()) return;
    const std::string bytes = readText(songs_[index].path);
    VgmFile f;
    std::string error;
    if (!f.parse(std::vector<uint8_t>(bytes.begin(), bytes.end()), &error)) {
        message_ = songs_[index].name + ": " + error;
        return;
    }
    playSynth_ = on ? &synth : nullptr;
    player_.load(std::move(f));
    player_.setSynth(playSynth_, chipPercent);
    current_ = index;
    playing_ = true;
}

void InstrumentEditor::stopSong() {
    if (!playing_) return;
    playing_ = false;
    player_.setSynth(nullptr, 100);
    if (playSynth_) playSynth_->allNotesOff();
    playSynth_ = nullptr;
}

void InstrumentEditor::play(double seconds, double rate, std::vector<int16_t>& out) {
    if (!playing_) return;
    player_.play(seconds, rate, out);
    if (player_.finished()) stopSong();
}

void InstrumentEditor::stopRecording(Console* console, bool keep) {
    if (!recording_) return;
    if (console == recording_) console->recordMusic(nullptr);
    recording_ = nullptr;
    if (!keep) return;
    const std::string dir = tracksFor(folder_, game_);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    std::string path, name;
    for (int n = 1; n < 1000; ++n) {
        name = trf("Grabación %02d", n);
        path = dir + name + ".vgz";
        if (!std::filesystem::exists(std::filesystem::u8path(path), ec)) break;
    }
    const std::vector<uint8_t> file =
        writer_.finish({name, console ? console->title() : std::string(), tr("Grabado jugando con plumnin")});
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    message_ = out ? trf("Guardada como %s", path.c_str()) : trf("No se pudo guardar %s", path.c_str());
    scanned_ = false;
}

void InstrumentEditor::finishRip() {
    ripThread_.join();
    ripDone_ = false;
    if (!ripOk_) {
        message_ = ripError_;
    } else {
        const std::string dir = tracksFor(folder_, ripGame_);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        int saved = 0;
        for (size_t i = 0; i < ripSongs_.size(); ++i) {
            const std::string path = dir + trf("Canción %02d", static_cast<int>(i + 1)) + ".vgz";
            std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
            const auto& v = ripSongs_[i].vgz;
            out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
            if (out) ++saved;
        }
        message_ = trf("%d canciones guardadas en %s", saved, dir.c_str());
    }
    ripSongs_.clear();
    ripProgress_.reset();
    scanned_ = false;
}

void InstrumentEditor::drawSongs(SynthMusic& synth, bool on, Console* console, int chipPercent) {
    ImGui::TextWrapped("%s", tr("Las canciones del juego, para oírlas sin jugar: el juego espera mientras suena una. Con el "
                                "sintetizador puesto suenan con él, y en Instrumentos puedes cambiar los suyos mientras suenan."));
    const bool gameBoy = console && (console->system() == ConsoleSystem::GameBoy ||
                                     console->system() == ConsoleSystem::GameBoyColor);
    const bool ripping = ripThread_.joinable();

    // Sacar canciones.
    if (ripping) {
        const GbSongRipper::Progress& p = *ripProgress_;
        const float fraction = static_cast<float>(p.step.load()) / static_cast<float>(std::max(1, p.steps.load()));
        const std::string label = trf("%d canciones encontradas", p.found.load());
        ImGui::ProgressBar(fraction, ImVec2(300, 0), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancelar"))) ripProgress_->cancel = true;
    } else {
        ImGui::BeginDisabled(!gameBoy || recording_);
        if (ImGui::Button(tr("Sacar canciones de la ROM"))) {
            std::vector<uint8_t> rom;
            std::string name, error;
            if (!readRomFile(game_, rom, &name, &error)) {
                message_ = error;
            } else {
                stopSong();
                message_.clear();
                ripProgress_ = std::make_unique<GbSongRipper::Progress>();
                ripGame_ = game_;
                ripDone_ = false;
                GbSongRipper::Progress* progress = ripProgress_.get();
                ripThread_ = std::thread([this, rom = std::move(rom), progress] {
                    std::string e;
                    ripOk_ = GbSongRipper::run(rom, {}, progress, &ripSongs_, &e);
                    ripError_ = e;
                    ripDone_ = true;
                });
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", gameBoy ? tr("Busca cómo pide el juego su música y graba cada canción con su bucle. "
                                                 "Suele tardar menos de un minuto, y en algunos juegos unos pocos.")
                                            : tr("Por ahora, solo en los juegos de Game Boy y Game Boy Color."));
    }
    ImGui::SameLine();
    if (recording_) {
        if (ImGui::Button(tr("Parar y guardar la grabación"))) stopRecording(console, true);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", trf("Grabando %.0f s", writer_.seconds()).c_str());
    } else {
        ImGui::BeginDisabled(!console || !console->musicRecordable() || ripping);
        if (ImGui::Button(tr("Grabar la música del juego"))) {
            stopSong();
            if (console->recordMusic(&writer_)) {
                recording_ = console;
                message_ = tr("Juega: se graba lo que suene hasta que la pares.");
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Abrir la carpeta"))) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(tracksFor(folder_, game_)), ec);
        openInSystem(tracksFor(folder_, game_));
        scanned_ = false;
    }
    if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("canciones", 4, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Canción"));
        ImGui::TableSetupColumn(tr("Dura"), ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(tr("Bucle"), ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        auto time = [](double s) {
            const int t = static_cast<int>(s + 0.5);
            char b[16];
            std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
            return std::string(b);
        };
        for (size_t i = 0; i < songs_.size(); ++i) {
            const Track& t = songs_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool now = playing_ && current_ == i;
            if (now) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s", t.name.c_str());
            else ImGui::TextUnformatted(t.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.path.c_str());
            ImGui::TableNextColumn();
            if (now) ImGui::Text("%s / %s", time(player_.position()).c_str(), time(t.seconds).c_str());
            else ImGui::TextUnformatted(time(t.seconds).c_str());
            ImGui::TableNextColumn();
            if (t.loopSeconds > 0) ImGui::TextUnformatted(time(t.loopSeconds).c_str());
            else ImGui::TextDisabled("%s", tr("no"));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(recording_ != nullptr);
            if (now) {
                if (ImGui::SmallButton(tr("Parar"))) stopSong();
            } else if (ImGui::SmallButton(tr("Escuchar"))) {
                startSong(i, synth, on, chipPercent);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (songs_.empty())
        ImGui::TextDisabled("%s", gameBoy ? tr("Todavía no hay canciones: sácalas de la ROM o graba la música jugando.")
                                          : tr("Todavía no hay canciones."));
}

}  // namespace plumnin
