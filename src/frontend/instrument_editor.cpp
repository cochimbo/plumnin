#include "instrument_editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "imgui.h"
#include "file_dialog.h"
#include "miniz.h"
#include "../core/console.h"
#include "../core/i18n.h"

namespace plumnin {

namespace {

namespace fs = std::filesystem;

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string readText(const std::string& path) {
    std::vector<uint8_t> data;
    if (!readBytes(path, data)) return {};
    return std::string(data.begin(), data.end());
}

bool writeBytes(const std::string& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::string fileName(const std::string& path) { return fs::u8path(path).filename().u8string(); }
std::string stem(const std::string& path) { return fs::u8path(path).stem().u8string(); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& name, const char* ext) {
    const std::string l = lower(name);
    const size_t n = std::char_traits<char>::length(ext);
    return l.size() > n && l.compare(l.size() - n, n, ext) == 0;
}
bool isTrack(const std::string& name) {
    return endsWith(name, ".vgm") || endsWith(name, ".vgz") || endsWith(name, ".spc");
}
bool isZip(const std::string& name) { return endsWith(name, ".zip"); }

// The songs in a .zip, by name, with their contents.
std::vector<std::pair<std::string, std::vector<uint8_t>>> zipTracks(const std::vector<uint8_t>& zip) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) return out;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory || !isTrack(st.m_filename)) continue;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        if (!data) continue;
        const uint8_t* b = static_cast<const uint8_t*>(data);
        out.emplace_back(st.m_filename, std::vector<uint8_t>(b, b + size));
        mz_free(data);
    }
    mz_zip_reader_end(&archive);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// What a song is called: its tag's title, else its file name.
std::string trackTitle(std::vector<uint8_t> bytes, const std::string& name) {
    VgmFile file;
    if (file.parse(std::move(bytes), nullptr) && !file.title.empty()) return file.title;
    return stem(name);
}

std::string noteName(int note) {
    static const char* kNames[12] = {N_("Do"), N_("Do#"), N_("Re"), N_("Re#"), N_("Mi"), N_("Fa"),
                                     N_("Fa#"), N_("Sol"), N_("Sol#"), N_("La"), N_("La#"), N_("Si")};
    note = std::clamp(note, 0, 127);
    return std::string(tr(kNames[note % 12])) + std::to_string(note / 12 - 1);
}

std::string minutes(double seconds) {
    const int s = static_cast<int>(std::max(0.0, seconds));
    char text[16];
    std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
    return text;
}

const char* const kFamilies[16] = {N_("Pianos"), N_("Percusión afinada"), N_("Órganos"), N_("Guitarras"), N_("Bajos"), N_("Cuerdas"),
                                   N_("Conjuntos"), N_("Metales"), N_("Lengüetas"), N_("Flautas"), N_("Solistas sintetizados"),
                                   N_("Fondos sintetizados"), N_("Efectos sintetizados"), N_("Étnicos"), N_("Percusión"),
                                   N_("Efectos de sonido")};

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

// A, B ... Z, then AA, AB...
std::string instrumentLetter(int n) {
    std::string s;
    for (++n; n > 0; n = (n - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (n - 1) % 26));
    return s;
}

std::string describe(const SynthMusic::Assignment& a) {
    if (a.original) return tr("Sonido original");
    if (a.drum >= 0) return trf("Batería: %s", SynthMusic::drumName(a.drum));
    if (a.program < 0) return tr("Nada");
    std::string s = SynthMusic::programName(a.program);
    if (a.octave) s += trf(" (%+d oct.)", a.octave);
    return s;
}

// The console's channels, as the synthesizer counts them.
std::vector<std::string> voiceNames(ConsoleSystem system) {
    switch (system) {
        case ConsoleSystem::Nes: return {tr("Pulso 1"), tr("Pulso 2"), tr("Triángulo"), tr("Ruido")};
        case ConsoleSystem::Snes: {
            std::vector<std::string> v;
            for (int i = 1; i <= 8; ++i) v.push_back(trf("Voz %d", i));
            return v;
        }
        default: return {tr("Pulso 1"), tr("Pulso 2"), tr("Onda"), tr("Ruido")};
    }
}

constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
const ImVec4 kPlaying(0.45f, 0.95f, 0.55f, 1.0f);

}  // namespace

InstrumentEditor::InstrumentEditor() = default;

InstrumentEditor::~InstrumentEditor() {
    if (ripThread_.joinable()) {
        ripProgress_->cancel = true;
        ripThread_.join();
    }
}

std::string InstrumentEditor::fileFor(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return folder + stem(game) + ".ini";
}

SynthMusic::Assignments InstrumentEditor::load(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return SynthMusic::parseAssignments(readText(fileFor(folder, game)));
}

std::set<uint32_t> InstrumentEditor::effectsFor(const std::string& folder, const std::string& game) {
    std::set<uint32_t> out;
    if (game.empty()) return out;
    const std::string text = readText(folder + stem(game) + ".effects");
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == ';') continue;
        char* stop = nullptr;
        const unsigned long fp = std::strtoul(line.c_str(), &stop, 16);
        if (stop != line.c_str() && fp) out.insert(static_cast<uint32_t>(fp));
    }
    return out;
}

// A song's instruments, in its file: "instrumentos = huella huella...".
static std::set<uint32_t> songInstruments(const std::string& text) {
    std::set<uint32_t> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.compare(0, 12, "instrumentos") != 0) continue;
        const char* p = line.c_str() + eq + 1;
        for (;;) {
            char* stop = nullptr;
            const unsigned long fp = std::strtoul(p, &stop, 16);
            if (stop == p) break;
            if (fp) out.insert(static_cast<uint32_t>(fp));
            p = stop;
        }
    }
    return out;
}

std::string InstrumentEditor::songFile(const Track& track) {
    if (track.path.empty()) return {};
    const fs::path p = fs::u8path(track.path);
    const std::string dir = p.parent_path().u8string() + "/";
    if (track.member.empty()) return dir + p.stem().u8string() + ".ini";
    return dir + p.stem().u8string() + " - " + stem(track.member) + ".ini";
}

void InstrumentEditor::writeSong(const std::string& path, const SynthMusic::Assignments& assigned,
                                 const std::set<uint32_t>& instruments) {
    std::string text = SynthMusic::formatAssignments(assigned);
    text += "\n; " + std::string(tr("Los instrumentos de la canción, para saber cuándo suena en el juego.")) +
            "\n[cancion]\ninstrumentos =";
    char word[16];
    for (uint32_t fp : instruments) {
        std::snprintf(word, sizeof word, " %08x", fp);
        text += word;
    }
    text += "\n";
    writeBytes(path, text.data(), text.size());
}

std::vector<SynthMusic::Song> InstrumentEditor::songsFor(const std::string& folder, const std::string& game) {
    std::vector<SynthMusic::Song> out;
    if (game.empty()) return out;
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(tracksFor(folder, game)), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string p = it->path().u8string();
        if (endsWith(p, ".ini")) files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) {
        const std::string text = readText(f);
        SynthMusic::Song song;
        song.instruments = songInstruments(text);
        song.assigned = SynthMusic::parseAssignments(text);
        if (!song.instruments.empty()) out.push_back(std::move(song));
    }
    return out;
}

std::string InstrumentEditor::tracksFor(const std::string& folder, const std::string& game) {
    if (game.empty()) return {};
    return folder + "tracks/" + stem(game) + "/";
}

int InstrumentEditor::songsIn(const std::string& folder) {
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if ((isTrack(name) || isZip(name)) && it->is_regular_file(ec)) ++n;
    }
    return n;
}

const std::map<std::string, int>& InstrumentEditor::gamesWithMusic() {
    if (musicSeen_ == musicVersion_) return withMusic_;
    musicSeen_ = musicVersion_;
    withMusic_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(folder_ + "tracks/"), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec))
            if (const int n = songsIn(it->path().u8string())) withMusic_[it->path().filename().u8string()] = n;
    return withMusic_;
}

// ---------------------------------------------------------------------------------
// The game and its songs

void InstrumentEditor::setGame(const EditorView& view) {
    stop();
    game_ = view.game;
    gameName_ = game_.empty() ? std::string() : stem(game_);
    system_ = view.console ? view.console->system() : game_.empty() ? ConsoleSystem::GameBoy : detectSystem(game_);
    message_.clear();
    selected_ = soloInstrument_ = 0;
    mutedVoices_ = soloVoices_ = 0;
    silencedInstruments_.clear();
    own_ = load(folder_, game_);
    songOwn_.clear();
    synth_.setAssignments(own_);
    synth_.setSoundEffects(effectsFor(folder_, game_), view.effectsOriginal);
    listTracks();
    track_ = Track{};
    player_.load(VgmFile{});
    trackSeen_.clear();
    if (!tracks_.empty()) loadTrack(tracks_.front());
}

void InstrumentEditor::listTracks() {
    tracks_.clear();
    ++musicVersion_;
    if (gameName_.empty()) return;
    std::vector<std::string> songs;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(tracksFor(folder_, game_)), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string p = it->path().u8string();
        if (isTrack(p) || isZip(p)) songs.push_back(p);
    }
    std::sort(songs.begin(), songs.end());
    for (const std::string& p : songs) addTracks(p);
}

void InstrumentEditor::addTracks(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readBytes(path, bytes)) return;
    if (isZip(path)) {
        for (auto& [name, data] : zipTracks(bytes)) {
            Track t{path, name, trackTitle(std::move(data), name)};
            if (std::find(tracks_.begin(), tracks_.end(), t) == tracks_.end()) tracks_.push_back(t);
        }
        return;
    }
    Track t{path, {}, trackTitle(std::move(bytes), path)};
    if (std::find(tracks_.begin(), tracks_.end(), t) == tracks_.end()) tracks_.push_back(t);
}

bool InstrumentEditor::openTrack(const std::string& path) {
    stop();
    std::string from = path;
    // Kept with the game's songs, to be in its list next time.
    if (!gameName_.empty() && fs::u8path(path).parent_path() != fs::u8path(tracksFor(folder_, game_)).parent_path()) {
        std::vector<uint8_t> bytes;
        const std::string copy = tracksFor(folder_, game_) + fileName(path);
        if (readBytes(path, bytes) && writeBytes(copy, bytes.data(), bytes.size())) from = copy;
        ++musicVersion_;
    }
    const size_t before = tracks_.size();
    addTracks(from);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.path == from; });
    if (it == tracks_.end()) {
        message_ = isZip(path) ? trf("No se pudo abrir %s: no tiene ningún .vgm, .vgz ni .spc.", fileName(path).c_str())
                               : trf("No se pudo abrir %s.", fileName(path).c_str());
        return false;
    }
    return loadTrack(before < tracks_.size() ? tracks_[before] : *it);
}

bool InstrumentEditor::loadTrack(const Track& track) {
    stop();
    std::vector<uint8_t> bytes;
    if (!readBytes(track.path, bytes)) {
        message_ = trf("No se pudo leer %s.", track.path.c_str());
        return false;
    }
    if (!track.member.empty()) {
        std::vector<uint8_t> zip = std::move(bytes);
        bytes.clear();
        for (auto& [name, data] : zipTracks(zip))
            if (name == track.member) bytes = std::move(data);
    }
    VgmFile file;
    std::string error;
    if (!file.parse(std::move(bytes), &error)) {
        message_ = trf("No se pudo abrir %s: %s", track.title.c_str(), error.c_str());
        return false;
    }
    message_.clear();
    track_ = track;
    trackSeen_ = VgmPlayer::instruments(file);  // every instrument in it, before playing
    // What was chosen for this song alone; its file knows its instruments, for the game to
    // tell when it plays it.
    {
        const std::string path = songFile(track_);
        const std::string text = readText(path);
        songOwn_ = SynthMusic::parseAssignments(text);
        std::set<uint32_t> in;
        for (const auto& [fp, seen] : trackSeen_) in.insert(fp);
        if (!in.empty() && songInstruments(text) != in) writeSong(path, songOwn_, in);
    }
    synth_.setAssignments(chosen());
    // The rules by this song alone, as heard whole: from its first note, and not by how
    // other songs played its samples.
    synth_.forget();
    synth_.learn(trackSeen_);
    player_.load(std::move(file));
    player_.setLoop(loop_);
    soloInstrument_ = 0;
    silencedInstruments_.clear();
    return true;
}

ConsoleSystem InstrumentEditor::trackSystem() const {
    if (!live_ && player_.loaded())
        return player_.file().spc ? ConsoleSystem::Snes : player_.file().nesClock ? ConsoleSystem::Nes : ConsoleSystem::GameBoy;
    return system_;
}

void InstrumentEditor::stop() {
    playing_ = false;
    synth_.allNotesOff();
}

void InstrumentEditor::play(double seconds, double rate, std::vector<int16_t>& out) {
    if (!playing()) return;
    player_.play(seconds, rate, out);
    if (player_.finished()) {
        stop();
        player_.restart();
    }
}

// ---------------------------------------------------------------------------------
// The instruments

SynthMusic& InstrumentEditor::listened(const EditorView& view) {
    return live_ && view.gameSynth ? *view.gameSynth : synth_;
}

void InstrumentEditor::applyListening(const EditorView& view) {
    uint32_t muted = mutedVoices_;
    if (soloVoices_) muted |= SynthMusic::kAllVoices & ~soloVoices_;
    std::set<uint32_t> silenced = silencedInstruments_;
    SynthMusic& synth = listened(view);
    if (soloInstrument_) {
        for (const auto& [fp, seen] : live_ ? synth.seen() : trackSeen_)
            if (fp != soloInstrument_) silenced.insert(fp);
    }
    synth.setListening(muted, std::move(silenced));
}

SynthMusic::Assignments InstrumentEditor::chosen() const {
    SynthMusic::Assignments all = own_;
    if (songOpen())
        for (const auto& [fp, a] : songOwn_) all[fp] = a;
    return all;
}

void InstrumentEditor::save(const EditorView& view) {
    const std::string path = fileFor(folder_, game_);
    if (!path.empty()) {
        const std::string text = SynthMusic::formatAssignments(own_);
        message_ = writeBytes(path, text.data(), text.size()) ? std::string() : trf("No se pudo guardar %s", path.c_str());
    }
    if (songOpen()) {
        std::set<uint32_t> in;
        for (const auto& [fp, seen] : trackSeen_) in.insert(fp);
        writeSong(songFile(track_), songOwn_, in);
    }
    synth_.setAssignments(chosen());
    // Heard at once in the game, when it is the one running.
    if (view.console && view.gameSynth) {
        view.gameSynth->setAssignments(own_);
        view.gameSynth->setSongs(songsFor(folder_, game_));
    }
    applyListening(view);
}

void InstrumentEditor::assign(const EditorView& view, uint32_t fingerprint, const SynthMusic::Assignment* a) {
    if (a) target()[fingerprint] = *a;
    else target().erase(fingerprint);
    save(view);
}

void InstrumentEditor::resetAll(const EditorView& view) {
    target().clear();
    save(view);
}

// The instruments in the song (or heard in the game), and those the game's file assigns
// that it does not play: by kind, then in the order of the channel they play on.
std::vector<InstrumentEditor::Instrument> InstrumentEditor::instruments(const SynthMusic& synth) const {
    std::vector<Instrument> list;
    const std::map<uint32_t, SynthMusic::Seen>& seen = live_ ? synth.seen() : trackSeen_;
    for (const auto& [fp, s] : seen) list.push_back({fp, s, true, {}});
    for (const auto& [fp, a] : own_) {
        if (seen.count(fp)) continue;
        Instrument i;
        i.fingerprint = fp;
        i.seen.kind = SynthMusic::kindOf(fp);
        list.push_back(i);
    }
    auto lowest = [](const Instrument& i) {
        for (int v = 0; v < SynthMusic::kVoices; ++v)
            if ((i.seen.voices >> v) & 1) return v;
        return 99;
    };
    std::sort(list.begin(), list.end(), [&](const Instrument& a, const Instrument& b) {
        if (a.seen.kind != b.seen.kind) return a.seen.kind < b.seen.kind;
        if (a.heard != b.heard) return a.heard;
        if (lowest(a) != lowest(b)) return lowest(a) < lowest(b);
        return a.seen.order < b.seen.order;
    });
    int letters[8] = {};
    for (Instrument& i : list) {
        const int k = static_cast<int>(i.seen.kind) & 7;
        i.label = std::string(kindName(i.seen.kind)) + " " + instrumentLetter(letters[k]++);
    }
    return list;
}

// ---------------------------------------------------------------------------------
// The window

void InstrumentEditor::draw(const EditorView& view, EditorRequests& out) {
    if (ripDone_) finishRip();
    if (recording_ && recording_ != view.console) {
        // The game was closed while recording: its console is gone, and so is the recording.
        recording_ = nullptr;
        message_ = tr("La grabación se ha perdido: el juego se cerró antes de pararla.");
    }
    if (view.game != game_ || (game_.empty() && tracks_.empty() && musicSeen_ == 0)) setGame(view);
    chipPercent_ = view.chipPercent;
    // The SoundFont is the game's, shared (again when another is chosen).
    if (view.gameSynth && view.gameSynth->ready()) synth_.shareSoundfont(*view.gameSynth);
    synth_.setEffects(view.reverb, view.chorus);
    // The sound effects: again when Sacar canciones found them, or the option changed; the
    // game's too, when it is the one running.
    if (effectsVersion_ != effectsSeen_ || view.effectsOriginal != synth_.soundEffectsOriginal()) {
        effectsSeen_ = effectsVersion_;
        const std::set<uint32_t> effects = effectsFor(folder_, game_);
        synth_.setSoundEffects(effects, view.effectsOriginal);
        if (view.console && view.gameSynth) view.gameSynth->setSoundEffects(effects, view.effectsOriginal);
    }
    if (!open_) {
        if (playing_) stop();
        return;
    }
    // The game's instruments as it plays, when no song is open.
    const bool live = !player_.loaded() && view.console && view.synthOn && view.gameSynth;
    if (live != live_) {
        live_ = live;
        applyListening(view);
    }
    if (!synth_.ready()) synthOutput_ = false;
    player_.setSynth(&synth_, chipPercent_);
    player_.setSynthOutput(synthOutput_);

    const ImGuiViewport* screen = ImGui::GetMainViewport();
    const float top = ImGui::GetFrameHeight() + 10;  // below the menu bar
    ImGui::SetNextWindowPos(ImVec2(screen->Pos.x + 10, screen->Pos.y + top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(960.0f, screen->Size.x - 20), std::min(680.0f, screen->Size.y - top - 10)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Editor de instrumentos"), &open_)) {
        ImGui::End();
        if (!open_) stop();
        return;
    }
    if (!open_) stop();
    ImGui::Text(tr("Juego: %s"), gameName_.empty() ? tr("ninguno") : gameName_.c_str());
    drawTrack(view, out);
    if (!message_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", message_.c_str());
    ImGui::Separator();

    const std::vector<Instrument> list = instruments(listened(view));
    if (list.empty()) {
        if (player_.loaded())
            ImGui::TextWrapped("%s", tr("En esta pista no suena música."));
        else if (game_.empty())
            ImGui::TextWrapped("%s", tr("Carga un juego, o ábrelo desde su menú en la biblioteca: aquí saldrán sus "
                                        "canciones y sus instrumentos."));
        else
            ImGui::TextWrapped("%s", system_ == ConsoleSystem::Snes
                                         ? tr("Juega hasta que suene la canción que quieres probar y pulsa Guardar lo que "
                                              "suena, o abre un .spc con su música, de snesmusic.org por ejemplo. Aquí saldrán "
                                              "sus instrumentos; también los que suenan mientras juegas con el sintetizador "
                                              "puesto.")
                                         : tr("Saca las canciones de la ROM del juego (Sacar canciones), graba un trozo del "
                                              "juego (Grabar del juego, y juega hasta que suene la música que quieres "
                                              "probar) o abre un .vgm con su música, de vgmrips.net por ejemplo. Aquí "
                                              "saldrán sus instrumentos; también los que suenan mientras juegas con el "
                                              "sintetizador puesto."));
        ImGui::End();
        return;
    }
    // The lists take what the assignment below leaves.
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 4 + ImGui::GetTextLineHeightWithSpacing() * 2;
    const float height = std::max(120.0f, ImGui::GetContentRegionAvail().y - bottom);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("instrumentos", ImVec2(width * 0.64f, height), ImGuiChildFlags_ResizeX);
    drawInstruments(view, list);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("canales", ImVec2(0, height));
    drawChannels(view, list);
    ImGui::EndChild();
    ImGui::Separator();
    drawAssignment(view, list);
    ImGui::End();
}

void InstrumentEditor::drawTrack(const EditorView& view, EditorRequests& out) {
    // Which song.
    ImGui::SetNextItemWidth(360);
    const std::string current = track_.path.empty() ? std::string(tr("(ninguna)")) : track_.title;
    if (ImGui::BeginCombo(tr("Pista"), current.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < tracks_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(tracks_[i].title.c_str(), tracks_[i] == track_)) {
                loadTrack(tracks_[i]);
                applyListening(view);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(system_ == ConsoleSystem::Snes ? tr("Abrir .spc...") : tr("Abrir .vgm..."))) {
        stop();
        out.chooseTrack = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Un .vgm o .vgz, o el .zip con todas las canciones del juego de vgmrips.net\n"
                                   "(de Super Nintendo, un .spc, o un .zip de ellos, de snesmusic.org).\n"
                                   "Se guarda con las pistas del juego, para tenerlo la próxima vez."));
    const bool ripping = ripThread_.joinable();
    if (!gameName_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Carpeta"))) {
            std::error_code ec;
            fs::create_directories(fs::u8path(tracksFor(folder_, game_)), ec);
            openInSystem(tracksFor(folder_, game_));
            listTracks();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(tr("La carpeta de las canciones de este juego: lo que pongas en ella (.vgm, .vgz,\n"
                                 ".spc o un .zip de ellos) sale en la lista.\n%s"),
                              tracksFor(folder_, game_).c_str());
        ImGui::SameLine();
        const bool rippable = SongRipper::supported(system_);
        ImGui::BeginDisabled(ripping || !rippable || recording_);
        if (ImGui::Button(tr("Sacar canciones"))) startRip();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", rippable ? tr("Busca cómo pide el juego su música y graba cada canción con su bucle\n"
                                                  "(Canción 01, Canción 02...), con las pistas del juego. También aprende\n"
                                                  "cuáles de sus instrumentos son efectos de sonido. Suele tardar menos\n"
                                                  "de un minuto, y en algunos juegos unos pocos.")
                                             : tr("Por ahora, solo en los juegos de Game Boy, Game Boy Color, NES y Super Nintendo."));
    }
    if (recording_ || !game_.empty()) ImGui::SameLine();
    if (recording_) {
        if (ImGui::Button(tr("Parar la grabación"))) stopRecording(view.console, true);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), tr("Grabando %s"), minutes(writer_.seconds()).c_str());
        ImGui::TextWrapped("%s", tr("Juega hasta que suene lo que quieres probar y pulsa Parar. Para usar el teclado en el "
                                    "juego, haz clic fuera de esta ventana."));
    } else if (view.console && view.console->system() == ConsoleSystem::Snes) {
        // The Super Nintendo's sound processor plays a song by itself: what it is playing
        // now, saved, goes on as the game would.
        ImGui::BeginDisabled(ripping);
        if (ImGui::Button(tr("Guardar lo que suena"))) {
            stop();
            saveSnapshot(view.console);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Guarda la canción que suena en el juego como un .spc (Grabación 01...) y la abre\n"
                                       "aquí: sigue sonando como en el juego, sin él."));
    } else if (view.console) {
        ImGui::BeginDisabled(!view.console->musicRecordable() || ripping);
        if (ImGui::Button(tr("Grabar del juego"))) {
            stop();
            if (view.console->recordMusic(&writer_)) {
                recording_ = view.console;
                message_.clear();
            }
        }
        ImGui::EndDisabled();
    } else if (!game_.empty()) {
        if (ImGui::Button(tr("Jugar para grabar"))) {
            stop();
            out.load = game_;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Abre el juego; luego pulsa Grabar del juego aquí."));
    }
    drawRip();

    // Transport.
    ImGui::BeginDisabled(!player_.loaded() || recording_);
    if (ImGui::Button(playing_ ? tr("Pausa") : tr("Tocar"), ImVec2(80, 0))) {
        playing_ = !playing_;
        if (!playing_) synth_.allNotesOff();
        if (playing_ && live_) {
            live_ = false;
            applyListening(view);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Al principio"))) {
        player_.restart();
        applyListening(view);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Repetir"), &loop_)) player_.setLoop(loop_);
    ImGui::SameLine();
    const float length = static_cast<float>(std::max(1.0, player_.file().seconds()));
    float pos = seekTo_ >= 0 ? seekTo_ : static_cast<float>(std::fmod(player_.position(), static_cast<double>(length)));
    ImGui::SetNextItemWidth(260);
    const std::string label = minutes(pos) + " / " + minutes(length);
    if (ImGui::SliderFloat("##posicion", &pos, 0.0f, length, label.c_str())) seekTo_ = pos;
    if (ImGui::IsItemDeactivatedAfterEdit() && seekTo_ >= 0) {
        player_.seek(seekTo_);
        applyListening(view);
        seekTo_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Original"), !synthOutput_)) {
        synthOutput_ = false;
        synth_.allNotesOff();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!synth_.ready());
    if (ImGui::RadioButton(tr("Sintetizador"), synthOutput_)) synthOutput_ = true;
    ImGui::EndDisabled();
    if (!synth_.ready() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("No se ha podido abrir el SoundFont: elige otro en la lista de al lado."));
    ImGui::EndDisabled();
    if (view.drawSoundfont) {  // which one the synthesizer plays, to compare them here
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("SoundFont"));
        ImGui::SameLine();
        view.drawSoundfont();
    }
    if (player_.loaded() && !player_.file().game.empty()) {
        if (view.drawSoundfont) ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s", player_.file().game.c_str(), player_.file().title.empty() ? "" : " - ",
                            player_.file().title.c_str());
    }
    if (live_) ImGui::TextDisabled("%s", tr("Sin pista abierta: los instrumentos que suenan en el juego hasta ahora."));
}

void InstrumentEditor::drawInstruments(const EditorView& view, const std::vector<Instrument>& list) {
    SynthMusic& synth = listened(view);
    ImGui::TextDisabled("%s", tr("Instrumentos (elige uno para cambiarlo)"));
    std::set<uint32_t> now;  // the instruments playing a note now
    const uint32_t active = synth.activeVoices();
    for (int v = 0; v < SynthMusic::kVoices; ++v)
        if ((active >> v) & 1) now.insert(synth.voiceInstrument(v));
    if (!selected_ || std::none_of(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; }))
        selected_ = list.front().fingerprint;
    if (!ImGui::BeginTable("lista", 5, kListFlags | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Instrumento"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn(tr("Suena como"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn(tr("Canales"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn(tr("Notas"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(tr("Oír"), ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    const std::vector<std::string> names = voiceNames(trackSystem());
    const bool sounding = playing_ || live_;
    const float row = ImGui::GetFrameHeight();
    for (const Instrument& i : list) {
        ImGui::PushID(static_cast<int>(i.fingerprint));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
        const SynthMusic::Assignment a = synth.assignmentFor(i.fingerprint);
        const bool byHand = own_.count(i.fingerprint) != 0 || (songOpen() && songOwn_.count(i.fingerprint) != 0);
        const bool on = sounding && now.count(i.fingerprint);
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, kPlaying);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));  // level with the other columns
        if (ImGui::Selectable(i.label.c_str(), selected_ == i.fingerprint,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row)))
            selected_ = i.fingerprint;
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %08x", tr("Huella"), i.fingerprint);
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(describe(a).c_str());
        if (!byHand) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", synth.isSoundEffect(i.fingerprint) ? tr("(efecto)") : tr("(sugerido)"));
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (i.heard) {
            // "Pulso 1, 2", not "Pulso 1, Pulso 2": it fits.
            std::string channels, kind;
            for (int v = 0; v < static_cast<int>(names.size()); ++v) {
                if (!((i.seen.voices >> v) & 1)) continue;
                const std::string& name = names[static_cast<size_t>(v)];
                const size_t space = name.find(' ');
                const std::string prefix = space == std::string::npos ? std::string() : name.substr(0, space + 1);
                if (!channels.empty()) channels += ", ";
                channels += !prefix.empty() && prefix == kind ? name.substr(space + 1) : name;
                kind = prefix;
            }
            ImGui::TextUnformatted(channels.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", i.seen.notes);
        } else {
            ImGui::TextDisabled("%s", tr("no suena en esta pista"));
        }
        if (on) ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(4);
        bool heard = !silencedInstruments_.count(i.fingerprint);
        if (ImGui::Checkbox("##oir", &heard)) {
            if (!silencedInstruments_.erase(i.fingerprint)) silencedInstruments_.insert(i.fingerprint);
            applyListening(view);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Quítalo para no oír este instrumento."));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void InstrumentEditor::drawChannels(const EditorView& view, const std::vector<Instrument>& list) {
    SynthMusic& synth = listened(view);
    ImGui::TextDisabled("%s", tr("Canales"));
    // The channels the chosen instrument plays on stand out.
    uint32_t chosen = 0;
    for (const Instrument& i : list)
        if (i.fingerprint == selected_) chosen = i.seen.voices;
    const uint32_t active = synth.activeVoices();
    const bool sounding = playing_ || live_;
    bool changed = false;
    const std::vector<std::string> names = voiceNames(trackSystem());
    // Room left below the table for Oír todos.
    const float height =
        std::max(ImGui::GetFrameHeight() * 3, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("canales", 3, kListFlags, ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Canal"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Solo"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Silencio"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const float row = ImGui::GetFrameHeight();
        for (int v = 0; v < static_cast<int>(names.size()); ++v) {
            ImGui::PushID(v);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
            const bool on = sounding && ((active >> v) & 1);
            if ((chosen >> v) & 1) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(on ? kPlaying : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", names[static_cast<size_t>(v)].c_str());
            bool solo = (soloVoices_ >> v) & 1, mute = (mutedVoices_ >> v) & 1;
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Checkbox("##solo", &solo)) {
                soloVoices_ ^= 1u << v;
                changed = true;
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Checkbox("##silencio", &mute)) {
                mutedVoices_ ^= 1u << v;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(tr("Oír todos"))) {
        soloVoices_ = mutedVoices_ = 0;
        changed = true;
    }
    if (changed) applyListening(view);
}

void InstrumentEditor::drawAssignment(const EditorView& view, const std::vector<Instrument>& list) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; });
    if (it == list.end()) return;
    SynthMusic& synth = listened(view);
    const Instrument& inst = *it;
    const bool noise = inst.seen.kind == SynthMusic::Kind::Noise;
    SynthMusic::Assignment a = synth.assignmentFor(inst.fingerprint);
    const SynthMusic::Assignment before = a;
    const SynthMusic::Assignment suggestion = synth.suggestionFor(inst.fingerprint);
    if (inst.heard)
        ImGui::Text(tr("%s: %d %s, de %s a %s. Sugerencia: %s."), inst.label.c_str(), inst.seen.notes,
                    inst.seen.notes == 1 ? tr("nota") : tr("notas"), noteName(inst.seen.low).c_str(),
                    noteName(inst.seen.high).c_str(), describe(suggestion).c_str());
    else
        ImGui::Text("%s", inst.label.c_str());
    if (synth.isSoundEffect(inst.fingerprint)) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Es un efecto de sonido del juego."));
    }

    // Where the change goes: the song open alone, or the whole game.
    if (songOpen()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Cambiar en:"));
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("Solo esta canción"), forSong_)) forSong_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Lo que cambies suena solo en esta canción: en el juego, cuando suena ella\n"
                                       "(se reconoce por sus instrumentos)."));
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("Todo el juego"), !forSong_)) forSong_ = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Lo que cambies suena en todas las canciones del juego que no tengan algo\n"
                                       "elegido para ellas solas."));
        if (songOwn_.count(inst.fingerprint)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("(elegido para esta canción)"));
        } else if (own_.count(inst.fingerprint)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("(elegido para todo el juego)"));
        }
    }

    int kind = a.original ? 2 : a.drum >= 0 ? 1 : 0;
    if (!noise) {
        if (ImGui::RadioButton(tr("Instrumento"), kind == 0)) kind = 0;
        ImGui::SameLine();
    }
    if (ImGui::RadioButton(tr("Batería"), kind == 1)) kind = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Sonido original"), kind == 2)) kind = 2;
    if (kind == 0 && (a.original || a.drum >= 0)) {
        a.original = false;
        a.drum = -1;
        if (a.program < 0) a.program = inst.seen.rules.program >= 0 ? inst.seen.rules.program : 0;
    } else if (kind == 1 && (a.drum < 0 || a.original)) {
        a.original = false;
        a.drum = inst.seen.rules.drum >= 0 ? inst.seen.rules.drum : 38;
        a.program = -1;
    } else if (kind == 2) {
        a.original = true;
    }

    ImGui::BeginDisabled(kind == 2);
    ImGui::SetNextItemWidth(300);
    if (kind == 0) {
        const std::string preview = std::to_string(a.program + 1) + " " + SynthMusic::programName(a.program);
        if (ImGui::BeginCombo(tr("Instrumento GM"), preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int p = 0; p < 128; ++p) {
                if (p % 8 == 0) ImGui::SeparatorText(tr(kFamilies[p / 8]));
                const std::string name = std::to_string(p + 1) + " " + SynthMusic::programName(p);
                if (ImGui::Selectable(name.c_str(), p == a.program)) a.program = p;
                if (p == a.program && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt(tr("Octava"), &a.octave, -3, 3, "%+d");
    } else {
        const char* drum = SynthMusic::drumName(a.drum);
        if (ImGui::BeginCombo(tr("Pieza del kit"), drum ? drum : "", ImGuiComboFlags_HeightLarge)) {
            for (int n = 35; n <= 81; ++n)
                if (ImGui::Selectable(SynthMusic::drumName(n), n == a.drum)) a.drum = n;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderInt(tr("Volumen"), &a.volume, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    // Sliders save when let go, not on every step.
    const bool sliding = ImGui::IsItemActive();

    bool solo = soloInstrument_ == inst.fingerprint;
    if (ImGui::Checkbox(tr("Escuchar solo este"), &solo)) {
        soloInstrument_ = solo ? inst.fingerprint : 0;
        applyListening(view);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!target().count(inst.fingerprint));
    if (ImGui::Button(editingSong() && own_.count(inst.fingerprint) ? tr("Volver a lo del juego")
                                                                     : tr("Volver a la sugerencia"))) {
        assign(view, inst.fingerprint, nullptr);
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(target().empty());
    if (ImGui::Button(tr("Todo a las sugerencias..."))) ImGui::OpenPopup("todo");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", editingSong()
                                    ? tr("Quita todo lo elegido a mano para esta canción: cada instrumento vuelve a sonar\n"
                                         "como en todo el juego.")
                                    : tr("Quita todo lo elegido a mano para todo el juego: cada instrumento vuelve a sonar\n"
                                         "como sugieren las reglas (salvo en las canciones con algo elegido para ellas)."));
    if (ImGui::BeginPopup("todo")) {
        ImGui::TextUnformatted(editingSong() ? trf("¿Quitar todo lo elegido a mano en %s?", track_.title.c_str()).c_str()
                               : game_.empty() ? tr("¿Quitar todo lo elegido a mano?")
                                               : trf("¿Quitar todo lo elegido a mano en %s?", gameName_.c_str()).c_str());
        if (ImGui::Button(tr("Sí, todo a las sugerencias"))) {
            resetAll(view);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancelar"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (a != before || (!sliding && pendingSlide_)) {
        if (sliding) {
            // Heard at once, saved when the slider is let go.
            SynthMusic::Assignments all = chosen();
            all[inst.fingerprint] = a;
            synth.setAssignments(std::move(all));
            pendingSlide_ = true;
        } else {
            pendingSlide_ = false;
            assign(view, inst.fingerprint, &a);
        }
    }
}

// ---------------------------------------------------------------------------------
// Recording and Sacar canciones

void InstrumentEditor::stopRecording(Console* console, bool keep) {
    if (!recording_) return;
    if (console == recording_) console->recordMusic(nullptr);
    recording_ = nullptr;
    if (!keep) return;
    const std::string dir = tracksFor(folder_, game_);
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    std::string path, name;
    for (int n = 1; n < 1000; ++n) {
        name = trf("Grabación %02d", n);
        path = dir + name + ".vgz";
        if (!fs::exists(fs::u8path(path), ec)) break;
    }
    const std::vector<uint8_t> file =
        writer_.finish({name, console ? console->title() : std::string(), tr("Grabado jugando con plumnin")});
    if (!writeBytes(path, file.data(), file.size())) {
        message_ = trf("No se pudo guardar la grabación en %s.", path.c_str());
        return;
    }
    listTracks();
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.path == path; });
    if (it != tracks_.end()) loadTrack(*it);
}

void InstrumentEditor::saveSnapshot(Console* console) {
    const std::string dir = tracksFor(folder_, game_);
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    std::string path, name;
    for (int n = 1; n < 1000; ++n) {
        name = trf("Grabación %02d", n);
        path = dir + name + ".spc";
        if (!fs::exists(fs::u8path(path), ec)) break;
    }
    const std::vector<uint8_t> file = console->musicSnapshot(name, 0);
    if (file.empty()) return;
    if (!writeBytes(path, file.data(), file.size())) {
        message_ = trf("No se pudo guardar la grabación en %s.", path.c_str());
        return;
    }
    listTracks();
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.path == path; });
    if (it != tracks_.end()) loadTrack(*it);
}

void InstrumentEditor::startRip() {
    std::vector<uint8_t> rom;
    std::string name, error;
    if (!readRomFile(game_, rom, &name, &error)) {
        message_ = error;
        return;
    }
    stop();
    message_.clear();
    ripProgress_ = std::make_unique<SongRipper::Progress>();
    ripGame_ = game_;
    ripDone_ = false;
    ripEffects_.clear();
    SongRipper::Progress* progress = ripProgress_.get();
    ripThread_ = std::thread([this, rom = std::move(rom), progress] {
        std::string e;
        ripOk_ = SongRipper::run(rom, {}, progress, &ripSongs_, &e, nullptr, &ripEffects_);
        ripError_ = e;
        // Each song's instruments, for the game to tell which it plays.
        ripInstruments_.assign(ripSongs_.size(), {});
        for (size_t i = 0; ripOk_ && i < ripSongs_.size() && !progress->cancel; ++i) {
            VgmFile file;
            if (!file.parse(ripSongs_[i].vgz, nullptr)) continue;
            for (const auto& [fp, seen] : VgmPlayer::instruments(file)) ripInstruments_[i].insert(fp);
        }
        ripDone_ = true;
    });
}

void InstrumentEditor::drawRip() {
    if (!ripThread_.joinable() || !ripProgress_) return;
    const SongRipper::Progress& p = *ripProgress_;
    const float fraction = static_cast<float>(p.step.load()) / static_cast<float>(std::max(1, p.steps.load()));
    const int found = p.found.load();
    const std::string label = found == 1 ? std::string(tr("Probando: 1 canción")) : trf("Probando: %d canciones", found);
    ImGui::ProgressBar(fraction, ImVec2(360, 0), label.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Parar"))) ripProgress_->cancel = true;
}

void InstrumentEditor::finishRip() {
    ripThread_.join();
    ripDone_ = false;
    const bool cancelled = ripProgress_ && ripProgress_->cancel;
    ripProgress_.reset();
    if (!ripOk_) {
        if (!cancelled) message_ = ripError_;
        ripSongs_.clear();
        return;
    }
    const std::string dir = tracksFor(folder_, ripGame_);
    // The songs of an earlier Sacar canciones go: those it finds now replace them (a
    // song more or less, or found otherwise, and the old ones would linger in the list).
    {
        std::error_code ec;
        std::vector<fs::path> old;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            for (const char* ext : {".spc", ".vgz"})
                for (int n = 1; n < 1000; ++n)
                    if (name == trf("Canción %02d", n) + ext) old.push_back(it->path());
        }
        for (const fs::path& p : old) fs::remove(p, ec);
    }
    int saved = 0;
    for (size_t i = 0; i < ripSongs_.size(); ++i) {
        const std::string path =
            dir + trf("Canción %02d", static_cast<int>(i + 1)) + (ripSongs_[i].spc ? ".spc" : ".vgz");
        const auto& v = ripSongs_[i].vgz;
        if (writeBytes(path, v.data(), v.size())) ++saved;
        // What was chosen for it stays (the same game gives the same songs); its
        // instruments are the ones now.
        if (i < ripInstruments_.size() && !ripInstruments_[i].empty()) {
            const std::string ini = songFile(Track{path, {}, {}});
            writeSong(ini, SynthMusic::parseAssignments(readText(ini)), ripInstruments_[i]);
        }
    }
    ripSongs_.clear();
    ripInstruments_.clear();
    // The sound effects' instruments: they keep the chip's sound. (None found: the old
    // list goes, it was of other instruments.)
    if (ripEffects_.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(folder_ + stem(ripGame_) + ".effects"), ec);
    } else {
        std::string text = "; " + trf("Los efectos de sonido de %s (huellas de instrumentos), de Sacar canciones.",
                                      stem(ripGame_).c_str()) + "\n";
        char line[16];
        for (uint32_t fp : ripEffects_) {
            std::snprintf(line, sizeof line, "%08x\n", fp);
            text += line;
        }
        writeBytes(folder_ + stem(ripGame_) + ".effects", text.data(), text.size());
    }
    message_ = saved == 1 ? std::string(tr("He sacado 1 canción de la ROM: está en la lista de pistas."))
                          : trf("He sacado %d canciones de la ROM: están en la lista de pistas.", saved);
    if (ripGame_ == game_) {
        ++effectsVersion_;
        listTracks();
        // The song open may be one just written over (or gone): opened again from the disk.
        const Track open = track_;
        track_ = Track{};
        player_.load(VgmFile{});
        const auto same = std::find(tracks_.begin(), tracks_.end(), open);
        if (same != tracks_.end()) loadTrack(*same);
        else if (!tracks_.empty()) loadTrack(tracks_.front());
    } else {
        ++musicVersion_;
    }
}

}  // namespace plumnin
