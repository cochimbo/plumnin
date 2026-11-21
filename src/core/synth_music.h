#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace plumnin {

// The synthesizer: plays the music the console's sound chip is playing as General MIDI
// notes on a SoundFont, as a module like the Roland SC-55 would (plumsg's "Roland
// sound", for the Nintendo chips). The console polls its channels a few hundred times a
// second and hands over what each one is doing (sounding or not, its pitch, its loudness,
// which instrument); a new note is a note on, the pitch moves the pitch bend, the
// loudness the expression, and each instrument of the game gets a General MIDI one from
// some rules or by hand. It only adds sound: the console runs exactly the same with it
// or without it.
class SynthMusic {
public:
    // The console's channels, each a voice and a MIDI channel of its own (0-7: as many as
    // any console has); the drums go on MIDI channel 10.
    static constexpr int kVoices = 8;
    static constexpr int kDrums = 9;
    static constexpr uint32_t kAllVoices = (1u << kVoices) - 1;

    // What sort of channel plays an instrument, which the rules go by: a pulse (square)
    // wave, the Game Boy's wave channel, the NES triangle, noise, or a sample.
    enum class Kind : uint8_t { Pulse, Wave, Triangle, Noise, Sample };
    // An instrument is known by its fingerprint: its kind in the top four bits and a hash
    // of what makes it sound as it does (a pulse's duty and envelope, a wave's samples...).
    static uint32_t fingerprint(Kind kind, uint32_t hash) {
        return (static_cast<uint32_t>(kind) + 1) << 28 | (hash & 0x0FFFFFFFu);
    }
    static Kind kindOf(uint32_t fingerprint) {
        const uint32_t k = fingerprint >> 28;
        return k >= 1 && k <= 5 ? static_cast<Kind>(k - 1) : Kind::Pulse;
    }
    // FNV-1a, for the fingerprints.
    static uint32_t hash(const uint8_t* data, size_t size, uint32_t seed = 2166136261u);

    // What an instrument plays instead of its own sound.
    struct Assignment {
        int program = -1;       // General MIDI instrument 0-127
        int drum = -1;          // or a piece of the GM drum kit (note 35-81), on the drum channel
        bool original = false;  // or none: it keeps its own sound from the chip
        int octave = 0;         // -3..3, added to every note
        int volume = 100;       // percent of the note's own loudness
        bool operator==(const Assignment& o) const {
            return program == o.program && drum == o.drum && original == o.original && octave == o.octave &&
                   volume == o.volume;
        }
        bool operator!=(const Assignment& o) const { return !(*this == o); }
    };
    using Assignments = std::map<uint32_t, Assignment>;

    struct Traits;
    // What one of the chip's channels is doing when the console polls it.
    struct VoiceState {
        bool sounding = false;
        double note = 0;        // MIDI note, fractional (69 = A 440 Hz)
        double amplitude = 0;   // 0..1
        int pan = 64;           // 0 left .. 127 right
        uint32_t fingerprint = 0;
        uint32_t triggers = 0;  // the chip's key ons so far: a change is a new note
        // What the rules look at, by kind: a pulse's duty (0-3: 12.5, 25, 50, 75 %), a
        // noise's pitch (0 lowest .. 15 highest), a wave's 32 samples (0-15).
        int duty = 2;
        int noise = 8;
        const uint8_t* wave = nullptr;
        // Only a key on starts a note (the SNES keys every note on): a slide of the pitch
        // or a change of volume without one stays the same note.
        bool keyed = false;
        // A sample's traits (analyzeSample()), for Kind::Sample.
        const Traits* traits = nullptr;
        // The chip's own envelope on the note (0..1), where it has one apart from the
        // volume (the SNES ADSR): how the note swells or dies away, for the rules only.
        double envelope = -1;
    };

    // How the game has played an instrument so far, for the rules.
    struct Heard {
        int notes = 0;
        std::array<uint16_t, 128> pitches{};  // notes started, by MIDI note
        int distinct() const;
        int held = 0;            // notes let go
        double heldSeconds = 0;  // how long those were held, in all
        int decayed = 0;         // of those, whose loudness fell below a third (plucked)
        int swelled = 0;         // of those, that took more than a tenth of a second to swell
        void add(int note);
        int median() const;
        int span() const;  // semitones from its lowest note to its highest
    };
    // What the rules give an instrument: from its kind, what makes its sound (duty,
    // wave, noise pitch) and how it is played. Public for the tests.
    struct Traits {
        Kind kind = Kind::Pulse;
        int duty = 2;
        int noise = 8;
        std::array<uint8_t, 32> wave{};
        // A sample's: whether it has a pitch, whether it loops (an instrument that holds
        // its note) or how long it lasts, how bright it is (its spectrum's centre over the
        // highest frequency, 0..1) and how much of its sound is the fundamental (0..1).
        bool pitched = true, looped = true;
        double seconds = 1, brightness = 0.2, fundamental = 0.5;
        // Of its harmonics above the fundamental, how much are the odd ones (0..1): a
        // clarinet's, a square's, are nearly all odd.
        double odd = 0.5;
        // Without a pitch, the period that repeats least badly (0 if nothing does): the
        // note of a sound too rough for one, should the game play it as a tune (Star Fox's
        // brass in Corneria).
        double roughHz = 0;
        // How much sound a sample that dies away holds, struck once at full volume: the
        // seconds it would last at full scale (its squares' sum over the rate). 0 if it loops.
        double strike = 0;
    };
    static Assignment suggest(const Traits& traits, const Heard& heard);
    // What a sample sounds like: its traits, and the frequency of its note when played at
    // `rate` samples a second (0 if it has no pitch, like a drum). `loopStart` is where it
    // loops back to, if `looped`.
    static double analyzeSample(const int16_t* pcm, size_t count, size_t loopStart, bool looped, double rate,
                                Traits* traits);

    // An instrument heard since clearSeen(), for an editor.
    struct Seen {
        Kind kind = Kind::Pulse;
        uint32_t voices = 0;  // the voices that played it, as a mask
        int notes = 0;
        int low = 127, high = 0;
        int order = 0;  // when it was first heard: 0 the first instrument, 1 the next...
        Traits traits;  // what the rules saw of it
        Heard heard;    // and how it was played, as of its last note
        Assignment rules;
    };

    SynthMusic();
    ~SynthMusic();
    SynthMusic(const SynthMusic&) = delete;
    SynthMusic& operator=(const SynthMusic&) = delete;

    // A SoundFont 2 file's contents. False if it is not one.
    bool loadSoundfont(const std::vector<uint8_t>& data);
    bool ready() const;
    // The same SoundFont as `from`, sharing its samples (another player of the same
    // music, the instrument editor's). False if `from` has none.
    bool shareSoundfont(const SynthMusic& from);
    // Which SoundFont is loaded: it changes when another one is (or shared from another).
    const void* soundfontKey() const { return fontKey_; }

    void setEffects(int reverb, int chorus);  // 0..100 each
    // The equalizer: a gain in dB (-kEqRange..kEqRange) for each band, from the bass up.
    static constexpr int kEqBands = 5;
    static constexpr int kEqRange = 12;
    static constexpr double kEqFrequencies[kEqBands] = {100, 400, 1200, 3500, 9000};
    void setEqualizer(const std::array<int, kEqBands>& gains);
    void setVolume(int percent) { volume_ = percent; }

    // The instruments chosen by hand; the rest follow the rules. From each one's next note.
    void setAssignments(Assignments assigned);
    const Assignments& assignments() const { return assigned_; }
    Assignment assignmentFor(uint32_t fingerprint) const;
    // What it plays when not chosen by hand: the rules', or its own sound for an effect.
    Assignment suggestionFor(uint32_t fingerprint) const;
    // The game's sound effects (instruments, as Sacar canciones found them), and whether
    // they keep the chip's sound when not chosen by hand.
    void setSoundEffects(std::set<uint32_t> effects, bool original);
    bool isSoundEffect(uint32_t fingerprint) const { return effects_.count(fingerprint) != 0; }
    bool soundEffectsOriginal() const { return effectsOriginal_; }
    // The voices whose chip sound should be heard: those playing an instrument left
    // original.
    uint32_t chipVoices() const { return chipVoices_; }
    // The voices that should not be heard at all, from the chip either: those muted, or
    // playing an instrument silenced, for listening in the editor.
    uint32_t silentVoices() const { return silentVoices_; }
    void setListening(uint32_t mutedVoices, std::set<uint32_t> silenced);
    // The voices sounding a note now, and the instrument each one plays (0 for none yet).
    uint32_t activeVoices() const;
    uint32_t voiceInstrument(int voice) const { return voices_[static_cast<size_t>(voice & 7)].fingerprint; }
    const std::map<uint32_t, Seen>& seen() const { return seen_; }
    void clearSeen() { seen_.clear(); }

    // Assignment files: one line per instrument, "fingerprint = what [octave volume]",
    // what being a program (0-127), "bateria N" or "original". Lines that make no sense
    // are skipped. The same format as plumsg's.
    static Assignments parseAssignments(const std::string& text);
    static std::string formatAssignments(const Assignments& assignments);
    static const char* programName(int program);
    static const char* drumName(int note);  // 35-81, else nullptr

    // MIDI note (fractional) of a frequency in Hz; 0 for none.
    static double noteOf(double hz);

    // From the console, as it runs. The clocks are the console's own, `hz` a second.
    void setClock(double hz) { clockHz_ = hz; }
    void beginFrame(uint64_t clock);
    void update(int voice, const VoiceState& state, uint64_t clock);
    // Adds this frame's notes into `out` (interleaved stereo at `rate`), from sample
    // pair `from` to the end: they cover the frame that ends at `clock`.
    void endFrame(uint64_t clock, std::vector<int16_t>& out, size_t from, double rate);
    // Silences every note: after loading a savestate or rewinding, what was playing is
    // not what the console plays now.
    void allNotesOff();
    // Forgets the game's instruments as the rules heard them: for another game.
    void forget();
    // The instruments as another hearing had them, for the rules from the next note: the
    // editor's of a whole song, before it plays it (a game may play one sample as a drum
    // in a song and as a bass in another, Star Fox's in Corneria and in its menu).
    void learn(const std::map<uint32_t, Seen>& heard);

    // What it sent, for the tests: one line per event.
    void setLog(std::vector<std::string>* log) { log_ = log; }

private:
    enum class Ev : uint8_t { NoteOn, NoteOff, Program, Bend, Pan, Expression };
    struct Event {
        uint64_t clock;
        Ev type;
        uint8_t channel;
        int value;  // note, program, bend (0..16383), pan (0..127) or expression (0..127)
        int extra;  // note on: velocity
    };
    struct Voice {
        bool on = false;    // a note is sounding (on the chip)
        bool midi = false;  // and it went to the synth (not left original)
        bool drum = false;  // as a piece of the drum kit, which does not bend
        int note = 0;       // the MIDI note sent
        int octave = 0;     // semitones added to the chip's pitch
        int volume = 100;
        int pan = 64;
        uint32_t fingerprint = 0;
        uint32_t triggers = 0;
        double amplitude = 1.0;  // at note on, for the expression after it
        double lastNote = 0, lastAmplitude = 0, lowestRatio = 1;
        double peakEnvelope = 0;  // the envelope's highest on this note, and when
        uint64_t start = 0, peakAt = 0;
        // The envelope a moment after its peak, and the last while it sounded: how fast it
        // was dying away.
        double refEnvelope = -1, lastEnvelope = 0;
        uint64_t refAt = 0, lastAt = 0;
        uint64_t lastHit = 0;  // the noise's last strike, to tell a sweep from a roll
    };
    struct Rule {
        Traits traits;
        Heard heard;
        // The instrument it goes as, when it is another sound of the same channel's tune
        // (a duty or a wave the driver changes from note to note): the first one heard on
        // that channel. 0 for itself.
        uint32_t lead = 0;
    };
    // Each channel's tune: its first instrument, and its last note.
    struct Lead {
        uint32_t fingerprint = 0;
        uint64_t last = 0;
    };
    std::array<Lead, 8> leads_{};
    Assignment ruled(uint32_t fingerprint) const;
    struct Channel {
        int program = -1;
        int bend = 8192;
        int pan = 64;
        int expression = 127;
    };

    void emit(uint64_t clock, Ev type, int channel, int value, int extra = 0);
    void noteOn(int voice, const VoiceState& s, uint64_t clock);
    void noteOff(int voice, uint64_t clock);
    void pitchTo(int voice, double note, uint64_t clock);
    void expression(int voice, double amplitude, uint64_t clock);
    void panTo(int voice, int pan, uint64_t clock);
    static int velocity(const Voice& v, double amplitude);
    void updateChipVoices();
    void apply(const Event& e);
    void render(int16_t* out, int frames);

    struct Synth;
    std::unique_ptr<Synth> synth_;
    std::array<Voice, kVoices> voices_{};
    std::array<Channel, 16> channels_{};
    std::map<uint32_t, Rule> rules_;
    Assignments assigned_;
    std::set<uint32_t> effects_;
    bool effectsOriginal_ = false;
    const void* fontKey_ = nullptr;
    std::map<uint32_t, Seen> seen_;
    uint32_t chipVoices_ = 0, silentVoices_ = 0;
    uint32_t mutedVoices_ = 0;
    std::set<uint32_t> silenced_;
    std::vector<Event> events_;
    uint64_t frameStart_ = 0;
    int volume_ = 100;
    double clockHz_ = 1e6;
    std::vector<std::string>* log_ = nullptr;
};

}  // namespace plumnin
