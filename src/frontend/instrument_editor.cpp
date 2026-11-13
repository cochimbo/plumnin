#include "instrument_editor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "imgui.h"
#include "../core/i18n.h"

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

void InstrumentEditor::draw(SynthMusic& synth, const std::string& game, bool on) {
    if (game != game_) {
        game_ = game;
        error_.clear();
        if (solo_) solo(synth, 0);
    }
    if (!open_) {
        if (solo_) solo(synth, 0);
        return;
    }
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
    if (!on) {
        ImGui::TextWrapped("%s", tr("Elige Sonido > Sintetizador para que la música suene con el SoundFont; entonces sus "
                                    "instrumentos aparecen aquí a medida que suenan."));
        ImGui::End();
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
    ImGui::End();
}

}  // namespace plumnin
