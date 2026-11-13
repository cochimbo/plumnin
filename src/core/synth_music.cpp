#include "synth_music.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "i18n.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace plumnin {

namespace {

constexpr int kBendRange = 12;  // semitones either way, so slides stay one note

// A Schroeder reverb (four combs and two all-pass filters per side, Freeverb's
// tunings) and a chorus (one delay line swept by a slow sine), like the SC-55's
// own effects. Both are sends: a part of the dry sound goes in, the result is added.
class Effects {
public:
    void setRate(double rate) {
        if (rate == rate_) return;
        rate_ = rate;
        const double scale = rate / 44100.0;
        static constexpr int kCombs[4] = {1116, 1188, 1277, 1356};
        static constexpr int kAllPasses[2] = {556, 441};
        for (int side = 0; side < 2; ++side) {
            const int spread = side ? 23 : 0;
            for (int i = 0; i < 4; ++i) combs_[side][i].assign(static_cast<size_t>((kCombs[i] + spread) * scale), 0.0f);
            for (int i = 0; i < 2; ++i) allPasses_[side][i].assign(static_cast<size_t>((kAllPasses[i] + spread) * scale), 0.0f);
        }
        combPos_ = {};
        allPassPos_ = {};
        combFilter_ = {};
        chorus_.assign(static_cast<size_t>(rate * 0.05) * 2, 0.0f);
        chorusPos_ = 0;
        phase_ = 0;
    }
    void setAmounts(int reverb, int chorus) {
        reverb_ = std::clamp(reverb, 0, 100) / 100.0f;
        chorusAmount_ = std::clamp(chorus, 0, 100) / 100.0f;
    }

    void process(float* buf, int frames) {
        if (rate_ <= 0) return;
        const size_t chorusLen = chorus_.size() / 2;
        for (int i = 0; i < frames; ++i) {
            float& l = buf[i * 2];
            float& r = buf[i * 2 + 1];
            // Chorus: a copy 10-20 ms late, its delay swaying at 0.6 Hz.
            if (chorusAmount_ > 0 && chorusLen > 0) {
                chorus_[chorusPos_ * 2] = l;
                chorus_[chorusPos_ * 2 + 1] = r;
                const double delay = rate_ * (0.015 + 0.005 * std::sin(phase_));
                phase_ += 2 * 3.14159265358979 * 0.6 / rate_;
                if (phase_ > 2 * 3.14159265358979) phase_ -= 2 * 3.14159265358979;
                double at = static_cast<double>(chorusPos_) - delay;
                while (at < 0) at += static_cast<double>(chorusLen);
                const size_t a = static_cast<size_t>(at) % chorusLen, b = (a + 1) % chorusLen;
                const float f = static_cast<float>(at - std::floor(at));
                const float cl = chorus_[a * 2] + (chorus_[b * 2] - chorus_[a * 2]) * f;
                const float cr = chorus_[a * 2 + 1] + (chorus_[b * 2 + 1] - chorus_[a * 2 + 1]) * f;
                chorusPos_ = (chorusPos_ + 1) % chorusLen;
                l += cr * chorusAmount_ * 0.5f;  // crossed, for width
                r += cl * chorusAmount_ * 0.5f;
            }
            if (reverb_ <= 0) continue;
            const float in = (l + r) * 0.015f * reverb_;
            float out[2] = {0, 0};
            for (int side = 0; side < 2; ++side) {
                for (int c = 0; c < 4; ++c) {
                    std::vector<float>& line = combs_[side][c];
                    size_t& pos = combPos_[side][c];
                    const float y = line[pos];
                    float& filter = combFilter_[side][c];
                    filter = y * 0.8f + filter * 0.2f;  // damping
                    line[pos] = in + filter * 0.84f;     // room size
                    pos = (pos + 1) % line.size();
                    out[side] += y;
                }
                for (int a = 0; a < 2; ++a) {
                    std::vector<float>& line = allPasses_[side][a];
                    size_t& pos = allPassPos_[side][a];
                    const float b = line[pos];
                    line[pos] = out[side] + b * 0.5f;
                    out[side] = b - out[side];
                    pos = (pos + 1) % line.size();
                }
            }
            l += out[0] * 3.0f;
            r += out[1] * 3.0f;
        }
    }

private:
    double rate_ = 0;
    float reverb_ = 0.4f, chorusAmount_ = 0.2f;
    std::array<std::array<std::vector<float>, 4>, 2> combs_;
    std::array<std::array<std::vector<float>, 2>, 2> allPasses_;
    std::array<std::array<size_t, 4>, 2> combPos_{};
    std::array<std::array<size_t, 2>, 2> allPassPos_{};
    std::array<std::array<float, 4>, 2> combFilter_{};
    std::vector<float> chorus_;
    size_t chorusPos_ = 0;
    double phase_ = 0;
};

// The equalizer: a low shelf, three peaks and a high shelf (RBJ's biquads), one gain
// each in dB. A band at 0 dB is left out.
class Equalizer {
public:
    void setRate(double rate) {
        rate_ = rate;
        design();
    }
    void setGains(const std::array<int, SynthMusic::kEqBands>& gains) {
        if (gains == gains_) return;
        gains_ = gains;
        design();
    }

    void process(float* buf, int frames) {
        for (int b = 0; b < SynthMusic::kEqBands; ++b) {
            if (!gains_[static_cast<size_t>(b)] || rate_ <= 0) continue;
            Band& f = bands_[static_cast<size_t>(b)];
            for (int i = 0; i < frames; ++i)
                for (int side = 0; side < 2; ++side) {
                    float& x = buf[i * 2 + side];
                    float* z = f.z[side];
                    const float y = f.b0 * x + z[0];
                    z[0] = f.b1 * x - f.a1 * y + z[1];
                    z[1] = f.b2 * x - f.a2 * y;
                    x = y;
                }
        }
    }

private:
    struct Band {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};
    };
    void design() {
        if (rate_ <= 0) return;
        for (int b = 0; b < SynthMusic::kEqBands; ++b) {
            Band& f = bands_[static_cast<size_t>(b)];
            const double gain = std::pow(10.0, gains_[static_cast<size_t>(b)] / 40.0);
            const double w = 2 * 3.14159265358979 * std::min(SynthMusic::kEqFrequencies[b], rate_ * 0.45) / rate_;
            const double cw = std::cos(w), sw = std::sin(w);
            double b0, b1, b2, a0, a1, a2;
            if (b == 0 || b == SynthMusic::kEqBands - 1) {  // shelves, slope 1
                const double alpha = sw / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(gain) * alpha;
                const double sign = b == 0 ? 1 : -1;  // the high shelf is the low one with cos negated
                b0 = gain * ((gain + 1) - sign * (gain - 1) * cw + sq);
                b1 = 2 * sign * gain * ((gain - 1) - sign * (gain + 1) * cw);
                b2 = gain * ((gain + 1) - sign * (gain - 1) * cw - sq);
                a0 = (gain + 1) + sign * (gain - 1) * cw + sq;
                a1 = -2 * sign * ((gain - 1) + sign * (gain + 1) * cw);
                a2 = (gain + 1) + sign * (gain - 1) * cw - sq;
            } else {  // peaks, Q 1: about an octave and a half wide
                const double alpha = sw / 2;
                b0 = 1 + alpha * gain;
                b1 = -2 * cw;
                b2 = 1 - alpha * gain;
                a0 = 1 + alpha / gain;
                a1 = -2 * cw;
                a2 = 1 - alpha / gain;
            }
            f.b0 = static_cast<float>(b0 / a0);
            f.b1 = static_cast<float>(b1 / a0);
            f.b2 = static_cast<float>(b2 / a0);
            f.a1 = static_cast<float>(a1 / a0);
            f.a2 = static_cast<float>(a2 / a0);
        }
    }

    double rate_ = 0;
    std::array<int, SynthMusic::kEqBands> gains_{};
    std::array<Band, SynthMusic::kEqBands> bands_{};
};


const char* const kPrograms[128] = {
    N_("Piano de cola"), N_("Piano brillante"), N_("Piano eléctrico de cola"), N_("Piano de cantina"), N_("Piano eléctrico 1"),
    N_("Piano eléctrico 2"), N_("Clavecín"), N_("Clavinet"), N_("Celesta"), N_("Glockenspiel"), N_("Caja de música"), N_("Vibráfono"),
    N_("Marimba"), N_("Xilófono"), N_("Campanas tubulares"), N_("Dulcémele"), N_("Órgano de barras"), N_("Órgano de percusión"),
    N_("Órgano de rock"), N_("Órgano de iglesia"), N_("Armonio"), N_("Acordeón"), N_("Armónica"), N_("Bandoneón"), N_("Guitarra de nailon"),
    N_("Guitarra de acero"), N_("Guitarra eléctrica de jazz"), N_("Guitarra eléctrica limpia"), N_("Guitarra eléctrica apagada"),
    N_("Guitarra saturada"), N_("Guitarra distorsionada"), N_("Armónicos de guitarra"), N_("Bajo acústico"), N_("Bajo eléctrico (dedo)"),
    N_("Bajo eléctrico (púa)"), N_("Bajo sin trastes"), N_("Bajo slap 1"), N_("Bajo slap 2"), N_("Bajo sintetizado 1"),
    N_("Bajo sintetizado 2"), N_("Violín"), N_("Viola"), N_("Violonchelo"), N_("Contrabajo"), N_("Cuerdas en trémolo"),
    N_("Cuerdas en pizzicato"), N_("Arpa"), N_("Timbales"), N_("Cuerdas 1"), N_("Cuerdas 2"), N_("Cuerdas sintetizadas 1"),
    N_("Cuerdas sintetizadas 2"), N_("Coro \"aah\""), N_("Voz \"ooh\""), N_("Voz sintetizada"), N_("Golpe de orquesta"), N_("Trompeta"),
    N_("Trombón"), N_("Tuba"), N_("Trompeta con sordina"), N_("Trompa"), N_("Sección de metales"), N_("Metales sintetizados 1"),
    N_("Metales sintetizados 2"), N_("Saxo soprano"), N_("Saxo alto"), N_("Saxo tenor"), N_("Saxo barítono"), N_("Oboe"), N_("Corno inglés"),
    N_("Fagot"), N_("Clarinete"), N_("Flautín"), N_("Flauta"), N_("Flauta dulce"), N_("Flauta de pan"), N_("Botella soplada"), N_("Shakuhachi"),
    N_("Silbido"), N_("Ocarina"), N_("Solista cuadrada"), N_("Solista diente de sierra"), N_("Solista calíope"), N_("Solista chiff"),
    N_("Solista charango"), N_("Solista voz"), N_("Solista quintas"), N_("Solista bajo y melodía"), N_("Fondo new age"), N_("Fondo cálido"),
    N_("Fondo polisintetizador"), N_("Fondo coro"), N_("Fondo arco"), N_("Fondo metálico"), N_("Fondo halo"), N_("Fondo barrido"),
    N_("Efecto lluvia"), N_("Efecto banda sonora"), N_("Efecto cristal"), N_("Efecto atmósfera"), N_("Efecto brillo"),
    N_("Efecto duendes"), N_("Efecto ecos"), N_("Efecto ciencia ficción"), N_("Sitar"), N_("Banjo"), N_("Shamisen"), N_("Koto"), N_("Kalimba"),
    N_("Gaita"), N_("Violín celta"), N_("Shanai"), N_("Campanilla"), N_("Agogó"), N_("Tambores metálicos"), N_("Caja china"), N_("Taiko"),
    N_("Tom melódico"), N_("Tambor sintetizado"), N_("Platillo al revés"), N_("Traste de guitarra"), N_("Respiración"), N_("Olas"),
    N_("Pájaros"), N_("Teléfono"), N_("Helicóptero"), N_("Aplausos"), N_("Disparo")};

const char* const kDrumNames[47] = {
    N_("Bombo acústico"), N_("Bombo"), N_("Golpe de aro"), N_("Caja"), N_("Palmada"), N_("Caja eléctrica"), N_("Tom grave de suelo"),
    N_("Charles cerrado"), N_("Tom agudo de suelo"), N_("Charles con pedal"), N_("Tom grave"), N_("Charles abierto"),
    N_("Tom medio grave"), N_("Tom medio agudo"), N_("Platillo crash 1"), N_("Tom agudo"), N_("Platillo ride 1"), N_("Platillo chino"),
    N_("Campana del ride"), N_("Pandereta"), N_("Platillo splash"), N_("Cencerro"), N_("Platillo crash 2"), N_("Vibraslap"),
    N_("Platillo ride 2"), N_("Bongó agudo"), N_("Bongó grave"), N_("Conga aguda apagada"), N_("Conga aguda abierta"), N_("Conga grave"),
    N_("Timbal agudo"), N_("Timbal grave"), N_("Agogó agudo"), N_("Agogó grave"), N_("Cabasa"), N_("Maracas"), N_("Silbato corto"),
    N_("Silbato largo"), N_("Güiro corto"), N_("Güiro largo"), N_("Claves"), N_("Caja china aguda"), N_("Caja china grave"),
    N_("Cuíca apagada"), N_("Cuíca abierta"), N_("Triángulo apagado"), N_("Triángulo abierto")};


// The harmonics of a wave of 32 samples: how much of its energy is in the fundamental,
// and in the odd harmonics (a square or a clarinet has only those).
struct WaveShape {
    double fundamental = 1, odd = 1;
};
WaveShape waveShape(const std::array<uint8_t, 32>& wave) {
    double mean = 0;
    for (uint8_t s : wave) mean += s;
    mean /= 32;
    double total = 0, first = 0, odd = 0;
    for (int h = 1; h <= 16; ++h) {
        double re = 0, im = 0;
        for (int i = 0; i < 32; ++i) {
            const double a = 2 * 3.14159265358979 * h * i / 32;
            re += (wave[static_cast<size_t>(i)] - mean) * std::cos(a);
            im -= (wave[static_cast<size_t>(i)] - mean) * std::sin(a);
        }
        const double e = re * re + im * im;
        total += e;
        if (h == 1) first = e;
        if (h & 1) odd += e;
    }
    if (total <= 0) return {};
    return {first / total, odd / total};
}

}  // namespace

struct SynthMusic::Synth {
    tsf* font = nullptr;
    int rate = 0;  // the rate tsf renders at, fixed once set
    Effects effects;
    Equalizer equalizer;
    std::vector<float> buffer;

    ~Synth() {
        if (font) tsf_close(font);
    }
};

SynthMusic::SynthMusic() : synth_(std::make_unique<Synth>()) {}

SynthMusic::~SynthMusic() = default;

uint32_t SynthMusic::hash(const uint8_t* data, size_t size, uint32_t seed) {
    uint32_t h = seed;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

bool SynthMusic::loadSoundfont(const std::vector<uint8_t>& data) {
    tsf* font = data.empty() ? nullptr : tsf_load_memory(data.data(), static_cast<int>(data.size()));
    if (!font) return false;
    if (synth_->font) tsf_close(synth_->font);
    synth_->font = font;
    synth_->rate = 0;
    for (Voice& v : voices_) v.on = v.midi = false;
    channels_ = {};
    return true;
}

bool SynthMusic::ready() const { return synth_->font != nullptr; }

void SynthMusic::setEffects(int reverb, int chorus) { synth_->effects.setAmounts(reverb, chorus); }

void SynthMusic::setEqualizer(const std::array<int, kEqBands>& gains) {
    std::array<int, kEqBands> clamped = gains;
    for (int& g : clamped) g = std::clamp(g, -kEqRange, kEqRange);
    synth_->equalizer.setGains(clamped);
}

const char* SynthMusic::programName(int program) { return program >= 0 && program < 128 ? tr(kPrograms[program]) : ""; }

const char* SynthMusic::drumName(int note) { return note >= 35 && note <= 81 ? tr(kDrumNames[note - 35]) : nullptr; }

double SynthMusic::noteOf(double hz) { return hz > 0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0; }

void SynthMusic::Heard::add(int note) {
    ++notes;
    ++pitches[static_cast<size_t>(std::clamp(note, 0, 127))];
}

int SynthMusic::Heard::distinct() const {
    int count = 0;
    for (uint16_t p : pitches) count += p != 0;
    return count;
}

int SynthMusic::Heard::median() const {
    int count = 0;
    for (int n = 0; n < 128; ++n)
        if ((count += pitches[static_cast<size_t>(n)]) * 2 >= notes) return n;
    return 60;
}

// The rules, until an instrument is chosen by hand. The noise is the drums: the higher
// it is, the smaller the drum (bass drum, snare, hi-hat), and a long one is a cymbal.
// The tones are an orchestra: in the bass register a bass; a note that dies away by
// itself plucked (a guitar or a harp); otherwise by the wave's harmonics, a pulse being
// thinner the narrower it is (a 12.5% oboe, a 25% trumpet, a 50% square a clarinet), a
// wave with nearly only its fundamental a flute, and the NES triangle a flute too.
SynthMusic::Assignment SynthMusic::suggest(const Traits& t, const Heard& heard) {
    Assignment a;
    const double held = heard.held ? heard.heldSeconds / heard.held : 0.1;
    if (t.kind == Kind::Noise) {
        if (t.noise >= 11) a.drum = held > 0.5 ? 49 : held > 0.2 ? 46 : 42;
        else if (t.noise >= 6) a.drum = 38;
        else a.drum = 36;
        return a;
    }
    const int median = heard.notes ? heard.median() : 64;
    const bool plucked = heard.held >= 3 && heard.decayed * 3 >= heard.held * 2;
    switch (t.kind) {
        case Kind::Triangle: a.program = median < 55 ? 33 : 73; break;
        case Kind::Wave: {
            const WaveShape w = waveShape(t.wave);
            if (median < 50) a.program = w.fundamental > 0.8 ? 33 : 38;
            else if (plucked) a.program = 46;
            else if (w.fundamental > 0.85) a.program = 73;
            else if (w.odd > 0.9) a.program = 71;
            else a.program = 48;
            break;
        }
        case Kind::Sample: {
            // A sample without a pitch, or a short one the game plays on one or two
            // notes, is a drum, by how bright it is; the rest by how it holds its note.
            const bool drum = !t.pitched || (!t.looped && t.seconds < 0.35 && (heard.notes < 4 || heard.distinct() <= 2));
            if (drum) {
                a.drum = t.brightness < 0.06 ? 36 : t.brightness < 0.18 ? 38 : t.seconds > 0.6 ? 49 : 42;
                break;
            }
            if (median < 48) a.program = t.looped ? 33 : 32;
            else if (!t.looped) a.program = t.seconds < 0.5 ? 12 : median < 62 ? 24 : 0;
            else if (t.fundamental > 0.8) a.program = 73;
            else if (t.brightness > 0.25) a.program = 61;
            else a.program = 48;
            break;
        }
        default: {
            static constexpr int kByDuty[4] = {68, 56, 71, 56};
            if (median < 45) a.program = 38;
            else if (plucked) a.program = median < 62 ? 24 : 46;
            else a.program = kByDuty[t.duty & 3];
            break;
        }
    }
    return a;
}

SynthMusic::Assignment SynthMusic::assignmentFor(uint32_t fingerprint) const {
    if (auto a = assigned_.find(fingerprint); a != assigned_.end()) return a->second;
    auto r = rules_.find(fingerprint);
    if (r != rules_.end()) return suggest(r->second.traits, r->second.heard);
    Traits t;
    t.kind = kindOf(fingerprint);
    return suggest(t, Heard{});
}

void SynthMusic::setAssignments(Assignments assigned) {
    assigned_ = std::move(assigned);
    updateChipVoices();
}

void SynthMusic::updateChipVoices() {
    chipVoices_ = 0;
    silentVoices_ = mutedVoices_;
    for (int i = 0; i < kVoices; ++i) {
        const Voice& v = voices_[static_cast<size_t>(i)];
        if (v.fingerprint && assignmentFor(v.fingerprint).original) chipVoices_ |= 1u << i;
        if (v.fingerprint && silenced_.count(v.fingerprint)) silentVoices_ |= 1u << i;
    }
}

void SynthMusic::setListening(uint32_t mutedVoices, std::set<uint32_t> silenced) {
    mutedVoices_ = mutedVoices;
    silenced_ = std::move(silenced);
    // What sounds and should not stops now.
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        if (v.midi && ((mutedVoices_ >> i) & 1 || silenced_.count(v.fingerprint))) {
            if (!v.drum) emit(frameStart_, Ev::NoteOff, i, v.note);
            v.midi = false;
        }
    }
    updateChipVoices();
}

uint32_t SynthMusic::activeVoices() const {
    uint32_t mask = 0;
    for (int i = 0; i < kVoices; ++i)
        if (voices_[static_cast<size_t>(i)].on) mask |= 1u << i;
    return mask;
}

SynthMusic::Assignments SynthMusic::parseAssignments(const std::string& text) {
    Assignments out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (const size_t c = line.find(';'); c != std::string::npos) line.resize(c);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        char* stop = nullptr;
        const std::string key = line.substr(0, eq);
        const unsigned long fp = std::strtoul(key.c_str(), &stop, 16);
        if (stop == key.c_str() || fp == 0) continue;
        char word[32] = "";
        int a = 0, b = 0, c = 0;
        const char* value = line.c_str() + eq + 1;
        Assignment as;
        if (std::sscanf(value, " %31s", word) != 1) continue;
        if (std::string(word) == "original") {
            as.original = true;
        } else if (std::string(word) == "bateria") {
            const int n = std::sscanf(value, " %*s %d %d %d", &a, &b, &c);
            if (n < 1 || !drumName(a)) continue;
            as.drum = a;
            if (n >= 3) as.volume = std::clamp(c, 0, 200);
        } else {
            const int n = std::sscanf(value, " %d %d %d", &a, &b, &c);
            if (n < 1 || a < 0 || a > 127) continue;
            as.program = a;
            if (n >= 2) as.octave = std::clamp(b, -3, 3);
            if (n >= 3) as.volume = std::clamp(c, 0, 200);
        }
        out[static_cast<uint32_t>(fp)] = as;
    }
    return out;
}

std::string SynthMusic::formatAssignments(const Assignments& assignments) {
    std::string out =
        "; Instrumentos del sintetizador: huella = instrumento GM (0-127) octava volumen,\n"
        "; bateria nota 0 volumen (una pieza del kit, 35-81), u original.\n[instrumentos]\n";
    for (const auto& [fp, a] : assignments) {
        char line[160];
        if (a.original)
            std::snprintf(line, sizeof line, "%08x = original\n", fp);
        else if (a.drum >= 0)
            std::snprintf(line, sizeof line, "%08x = bateria %d 0 %d\n", fp, a.drum, a.volume);
        else
            std::snprintf(line, sizeof line, "%08x = %d %d %d\n", fp, a.program, a.octave, a.volume);
        out += line;
    }
    return out;
}

void SynthMusic::emit(uint64_t clock, Ev type, int channel, int value, int extra) {
    events_.push_back({clock, type, static_cast<uint8_t>(channel), value, extra});
    if (!log_) return;
    static const char* kNames[] = {"on", "off", "program", "bend", "pan", "expression"};
    char line[64];
    std::snprintf(line, sizeof line, "%d %s %d", channel, kNames[static_cast<int>(type)], value);
    log_->push_back(line);
}

void SynthMusic::beginFrame(uint64_t clock) { frameStart_ = clock; }

void SynthMusic::allNotesOff() {
    events_.clear();
    for (Voice& v : voices_) v.on = v.midi = false;
    channels_ = {};
    if (synth_->font && synth_->rate) {
        tsf_note_off_all(synth_->font);
        for (int ch = 0; ch < 16; ++ch) {
            tsf_channel_set_pitchwheel(synth_->font, ch, 8192);
            tsf_channel_set_pan(synth_->font, ch, 0.5f);
            tsf_channel_midi_control(synth_->font, ch, 11, 127);
        }
    }
}

void SynthMusic::forget() {
    allNotesOff();
    voices_ = {};
    rules_.clear();
    seen_.clear();
    updateChipVoices();
}

int SynthMusic::velocity(const Voice& v, double amplitude) {
    return std::clamp(static_cast<int>(std::lround(127 * std::sqrt(amplitude) * v.volume / 100.0)), 1, 127);
}

void SynthMusic::noteOn(int voice, const VoiceState& s, uint64_t clock) {
    noteOff(voice, clock);
    Voice& v = voices_[static_cast<size_t>(voice)];
    const uint32_t fp = s.fingerprint;
    // What the rules go by, and what the editor shows.
    Rule& rule = rules_[fp];
    rule.traits.kind = kindOf(fp);
    rule.traits.duty = s.duty;
    rule.traits.noise = s.noise;
    if (s.wave) std::copy(s.wave, s.wave + 32, rule.traits.wave.begin());
    if (s.traits) {
        rule.traits = *s.traits;
        rule.traits.kind = kindOf(fp);
    }
    const int pitch = static_cast<int>(std::lround(s.note));
    if (rule.traits.kind != Kind::Noise) rule.heard.add(pitch);
    else ++rule.heard.notes;
    const Assignment a = assignmentFor(fp);
    if (!seen_.count(fp)) seen_[fp].order = static_cast<int>(seen_.size()) - 1;
    Seen& seen = seen_[fp];
    seen.kind = rule.traits.kind;
    seen.traits = rule.traits;
    seen.voices |= 1u << voice;
    ++seen.notes;
    seen.low = std::min(seen.low, pitch);
    seen.high = std::max(seen.high, pitch);
    seen.rules = suggest(rule.traits, rule.heard);

    v.on = true;
    v.amplitude = std::max(s.amplitude, 1e-3);
    v.lowestRatio = 1;
    v.start = clock;
    if (v.fingerprint != fp) {
        v.fingerprint = fp;
        updateChipVoices();
    }
    v.drum = a.drum >= 0;
    const bool quiet = a.original || ((mutedVoices_ >> voice) & 1) || silenced_.count(fp);
    // The noise only plays drums.
    v.midi = !quiet && (v.drum || (a.program >= 0 && rule.traits.kind != Kind::Noise));
    if (!v.midi) return;
    v.volume = a.volume;
    if (v.drum) {
        v.note = a.drum;
        emit(clock, Ev::NoteOn, kDrums, v.note, velocity(v, v.amplitude));
        return;
    }
    Channel& c = channels_[static_cast<size_t>(voice)];
    if (a.program != c.program) {
        c.program = a.program;
        emit(clock, Ev::Program, voice, a.program);
    }
    panTo(voice, s.pan, clock);
    if (c.expression != 127) {
        c.expression = 127;
        emit(clock, Ev::Expression, voice, 127);
    }
    v.octave = 12 * a.octave;
    v.note = std::clamp(pitch + v.octave, 0, 127);
    pitchTo(voice, s.note, clock);
    emit(clock, Ev::NoteOn, voice, v.note, velocity(v, v.amplitude));
}

void SynthMusic::noteOff(int voice, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    if (v.on) {
        if (auto r = rules_.find(v.fingerprint); r != rules_.end()) {
            Heard& h = r->second.heard;
            ++h.held;
            if (clock > v.start) h.heldSeconds += static_cast<double>(clock - v.start) / clockHz_;
            if (v.lowestRatio < 0.34) ++h.decayed;
        }
    }
    // Drums ring out by themselves, as on a GM module.
    if (v.on && v.midi && !v.drum) emit(clock, Ev::NoteOff, voice, v.note);
    v.on = v.midi = false;
}

void SynthMusic::panTo(int voice, int pan, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    v.pan = pan;
    if (!v.midi || v.drum) return;
    Channel& c = channels_[static_cast<size_t>(voice)];
    if (pan == c.pan) return;
    c.pan = pan;
    emit(clock, Ev::Pan, voice, pan);
}

void SynthMusic::expression(int voice, double amplitude, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    if (v.on) v.lowestRatio = std::min(v.lowestRatio, amplitude / v.amplitude);
    if (!v.midi || v.drum) return;
    Channel& c = channels_[static_cast<size_t>(voice)];
    const int e = std::clamp(static_cast<int>(127 * std::sqrt(amplitude / v.amplitude)), 0, 127);
    if (e == c.expression) return;
    c.expression = e;
    emit(clock, Ev::Expression, voice, e);
}

void SynthMusic::pitchTo(int voice, double note, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    if (!v.midi || v.drum || note <= 0) return;
    note += v.octave;
    Channel& c = channels_[static_cast<size_t>(voice)];
    // Within the bend range the note bends; further, it is played again at the new note.
    if (std::fabs(note - v.note) > kBendRange) {
        emit(clock, Ev::NoteOff, voice, v.note);
        v.note = std::clamp(static_cast<int>(std::lround(note)), 0, 127);
        emit(clock, Ev::NoteOn, voice, v.note, velocity(v, v.amplitude));
    }
    const int bend = std::clamp(8192 + static_cast<int>(std::lround((note - v.note) * 8192.0 / kBendRange)), 0, 16383);
    if (bend != c.bend) {
        c.bend = bend;
        emit(clock, Ev::Bend, voice, bend);
    }
}

// The note of a sample: YIN's difference function (de Cheveigné and Kawahara) over a
// stretch of it, the loop repeated if it loops, which finds the period that best repeats.
// Where nothing repeats well it has no pitch. Then how bright it is (its spectrum's
// centre) and how much of it is the fundamental, from the harmonics of that period.
double SynthMusic::analyzeSample(const int16_t* pcm, size_t count, size_t loopStart, bool looped, double rate,
                                 Traits* traits) {
    Traits t;
    t.kind = Kind::Sample;
    t.looped = looped && loopStart < count;
    t.seconds = t.looped ? 10.0 : static_cast<double>(count) / rate;
    constexpr size_t kLength = 2048, kMaxLag = 1000, kMinLag = 8;
    std::vector<double> x(kLength, 0.0);
    if (t.looped) {
        const size_t loop = count - loopStart;
        for (size_t i = 0; i < kLength; ++i) {
            const size_t at = loopStart + (i < count - loopStart ? i : i % loop);
            x[i] = at < count ? pcm[at] : 0;
        }
        // The stretch before the loop too, when there is enough of it: the attack.
        if (loopStart >= kLength + count / 10)
            for (size_t i = 0; i < kLength; ++i) x[i] = pcm[count / 10 + i];
    } else {
        const size_t from = std::min(count / 10, count > kLength ? count - kLength : 0);
        for (size_t i = 0; i < kLength && from + i < count; ++i) x[i] = pcm[from + i];
    }
    double energy = 0;
    for (double v : x) energy += v * v;
    double hz = 0;
    if (energy > 1e3 && count >= 64) {
        const size_t window = kLength - kMaxLag;
        std::vector<double> d(kMaxLag + 1, 0.0);
        double running = 0;
        size_t best = 0;
        double bestValue = 1e9;
        for (size_t lag = 1; lag <= kMaxLag; ++lag) {
            double sum = 0;
            for (size_t j = 0; j < window; ++j) {
                const double diff = x[j] - x[j + lag];
                sum += diff * diff;
            }
            running += sum;
            d[lag] = running > 0 ? sum * static_cast<double>(lag) / running : 1;
        }
        for (size_t lag = kMinLag; lag <= kMaxLag; ++lag) {
            if (d[lag] < 0.15) {
                while (lag + 1 <= kMaxLag && d[lag + 1] < d[lag]) ++lag;
                best = lag;
                bestValue = d[lag];
                break;
            }
            if (d[lag] < bestValue) best = lag, bestValue = d[lag];
        }
        if (best && bestValue < 0.35) {
            double lag = static_cast<double>(best);
            if (best > kMinLag && best < kMaxLag) {  // between samples, by a parabola
                const double a = d[best - 1], b = d[best], c = d[best + 1];
                const double den = a - 2 * b + c;
                if (den > 0) lag += 0.5 * (a - c) / den;
            }
            hz = rate / lag;
        }
    }
    t.pitched = hz > 0;
    // The spectrum, by its DFT at a few hundred frequencies, for the brightness; and the
    // harmonics of the note for the fundamental's part.
    if (energy > 0) {
        double centre = 0, total = 0;
        for (int k = 1; k < 128; ++k) {
            const double f = k / 256.0;  // of the rate
            double re = 0, im = 0;
            for (size_t i = 0; i < kLength; i += 2) {
                const double a = 2 * 3.14159265358979 * f * static_cast<double>(i);
                re += x[i] * std::cos(a);
                im -= x[i] * std::sin(a);
            }
            const double e = re * re + im * im;
            centre += e * k / 128.0;
            total += e;
        }
        t.brightness = total > 0 ? centre / total : 0;
        if (hz > 0) {
            double first = 0, all = 0;
            for (int h = 1; h <= 12 && h * hz < rate / 2; ++h) {
                double re = 0, im = 0;
                for (size_t i = 0; i < kLength; ++i) {
                    const double a = 2 * 3.14159265358979 * h * hz * static_cast<double>(i) / rate;
                    re += x[i] * std::cos(a);
                    im -= x[i] * std::sin(a);
                }
                const double e = re * re + im * im;
                if (h == 1) first = e;
                all += e;
            }
            t.fundamental = all > 0 ? first / all : 0;
        }
    }
    if (traits) *traits = t;
    return hz;
}

// A new note is a key on, another instrument, or, as the Nintendo chips are often
// played without a key on for each note (the NES drivers that leave the high byte of
// the period alone so as not to click), the pitch jumping to another semitone at once
// or the loudness jumping up. A vibrato or a slide moves the pitch bend.
void SynthMusic::update(int voice, const VoiceState& s, uint64_t clock) {
    if (voice < 0 || voice >= kVoices) return;
    Voice& v = voices_[static_cast<size_t>(voice)];
    const bool triggered = s.triggers != v.triggers;
    v.triggers = s.triggers;
    const bool noise = kindOf(s.fingerprint) == Kind::Noise;
    const bool audible = s.sounding && s.amplitude > 0 && s.fingerprint && (noise || (s.note > 12 && s.note < 120));
    if (!audible) {
        noteOff(voice, clock);
        v.lastAmplitude = 0;
        v.lastNote = 0;
        return;
    }
    bool fresh = !v.on || triggered || s.fingerprint != v.fingerprint;
    if (!fresh && !s.keyed && s.amplitude > v.lastAmplitude * 1.9 && s.amplitude >= 0.1) fresh = true;
    if (!fresh && !s.keyed && !noise && std::fabs(s.note - v.lastNote) >= 0.9) fresh = true;
    v.lastNote = s.note;
    v.lastAmplitude = s.amplitude;
    if (fresh) {
        noteOn(voice, s, clock);
        return;
    }
    if (!noise) pitchTo(voice, s.note, clock);
    expression(voice, s.amplitude, clock);
    if (s.pan != v.pan) panTo(voice, s.pan, clock);
}

void SynthMusic::apply(const Event& e) {
    tsf* f = synth_->font;
    const int ch = e.channel;
    switch (e.type) {
        case Ev::NoteOn: tsf_channel_note_on(f, ch, e.value, static_cast<float>(e.extra) / 127.0f); break;
        case Ev::NoteOff: tsf_channel_note_off(f, ch, e.value); break;
        case Ev::Program: tsf_channel_set_presetnumber(f, ch, e.value, ch == kDrums); break;
        case Ev::Bend: tsf_channel_set_pitchwheel(f, ch, e.value); break;
        case Ev::Pan: tsf_channel_set_pan(f, ch, static_cast<float>(e.value) / 127.0f); break;
        case Ev::Expression: tsf_channel_midi_control(f, ch, 11, e.value); break;
    }
}

void SynthMusic::render(int16_t* out, int frames) {
    if (frames <= 0) return;
    std::vector<float>& buf = synth_->buffer;
    buf.assign(static_cast<size_t>(frames) * 2, 0.0f);
    tsf_render_float(synth_->font, buf.data(), frames, 0);
    synth_->effects.process(buf.data(), frames);
    synth_->equalizer.process(buf.data(), frames);
    // As loud as the chip's own sound of the same music, more or less; the peaks above
    // three quarters of full scale are rounded off rather than clipped.
    const float gain = 20000.0f * static_cast<float>(volume_) / 100.0f;
    constexpr float kKnee = 24000.0f, kRoom = 32767.0f - kKnee;
    for (int i = 0; i < frames * 2; ++i) {
        float v = static_cast<float>(out[i]) + buf[static_cast<size_t>(i)] * gain;
        const float m = std::fabs(v);
        if (m > kKnee) v = std::copysign(kKnee + kRoom * std::tanh((m - kKnee) / kRoom), v);
        out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(v), -32768, 32767));
    }
}

void SynthMusic::endFrame(uint64_t clock, std::vector<int16_t>& out, size_t from, double rate) {
    Synth& s = *synth_;
    if (!s.font) {
        events_.clear();
        frameStart_ = clock;
        return;
    }
    if (s.rate == 0) {
        // The output rate moves a little every frame to keep the queue level; the synth
        // keeps the first one, which is near enough.
        s.rate = static_cast<int>(std::lround(rate));
        tsf_set_output(s.font, TSF_STEREO_INTERLEAVED, s.rate, 0.0f);
        tsf_set_max_voices(s.font, 64);
        for (int ch = 0; ch < 16; ++ch) {
            tsf_channel_set_pitchrange(s.font, ch, static_cast<float>(kBendRange));
            tsf_channel_set_presetnumber(s.font, ch, 0, ch == kDrums);
        }
        channels_ = {};
        s.effects.setRate(s.rate);
        s.equalizer.setRate(s.rate);
    }
    const int frames = static_cast<int>((out.size() - std::min(out.size(), from * 2)) / 2);
    const double span = clock > frameStart_ ? static_cast<double>(clock - frameStart_) : 1.0;
    int done = 0;
    for (const Event& e : events_) {
        const double t = e.clock > frameStart_ ? static_cast<double>(e.clock - frameStart_) / span : 0.0;
        const int at = std::clamp(static_cast<int>(t * frames), done, frames);
        render(out.data() + (from + static_cast<size_t>(done)) * 2, at - done);
        done = at;
        apply(e);
    }
    render(out.data() + (from + static_cast<size_t>(done)) * 2, frames - done);
    events_.clear();
    frameStart_ = clock;
}

}  // namespace plumnin
