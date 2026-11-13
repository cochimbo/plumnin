#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// NEC's signal processors: the uPD7725 (the DSP-1, DSP-2, DSP-3 and DSP-4, each with
// its own program) and the uPD96050 (the ST010 and ST011). The program is not part of
// plumnin; it comes from the chip's firmware file. The game talks to the chip through
// two registers, the data register (DR) and the status register (SR).
//
// Each instruction takes one cycle and does several things at once: an ALU operation
// on accumulator A or B, a move from one register to another, and the data pointer's
// and ROM pointer's updates. A multiplier forms K*L after every instruction.
class NecDsp : public Coprocessor {
public:
    enum class Model : uint8_t { Upd7725, Upd96050 };
    // Where the registers are on the processor's bus.
    enum class Map : uint8_t {
        LoRom,      // banks 30-3F and B0-BF, 8000-FFFF: DR below C000, SR from it
        LoRomRam,   // the same from bank 20 (the boards with battery RAM)
        LoRomHigh,  // banks 60-6F and E0-EF, 0000-7FFF: DR below 4000 (LoROM of 2 MB and more)
        HiRom,      // banks 00-1F and 80-9F, 6000-7FFF: DR below 7000
        St01x,      // banks 60-67 and E0-E7: DR at even, SR at odd; data RAM at 68-6F
    };

    // The firmware's size for that model: the program (3 bytes an instruction) then the
    // data ROM (2 bytes a word).
    static size_t firmwareSize(Model m) { return m == Model::Upd7725 ? 2048 * 3 + 1024 * 2 : 16384 * 3 + 2048 * 2; }
    // False if the firmware is not that size.
    bool setFirmware(Model model, Map map, const std::vector<uint8_t>& firmware, double hz);

    void power() override;
    void reset() override;
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t clocks) override { budget_ += static_cast<int64_t>(clocks) * hz_; }
    void setMasterRate(double hz) override { masterHz_ = static_cast<int64_t>(hz); }
    void catchUp() override;
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }
    std::vector<uint8_t> batteryData() const override;
    void loadBatteryData(const std::vector<uint8_t>& data) override;

    // For tests: runs instructions directly, and the registers the host sees.
    void step();
    uint16_t dr() const { return dr_; }
    uint16_t sr() const { return sr_; }
    uint16_t dataRam(int i) const { return dataRam_[static_cast<size_t>(i) & dpMask_]; }
    // An already decoded program, for tests (instructions and data ROM words).
    void loadProgram(Model model, Map map, const std::vector<uint32_t>& program, const std::vector<uint16_t>& data,
                     double hz);

    uint8_t readDr();
    void writeDr(uint8_t value);
    uint8_t readSr() const { return static_cast<uint8_t>(srValue() >> 8); }

private:
    struct Flags {
        bool ov0 = false, ov1 = false, z = false, c = false, s0 = false, s1 = false;
        template <class Ar>
        void state(Ar& ar) {
            ar(ov0, ov1, z, c, s0, s1);
        }
    };
    // Status register bits.
    static constexpr uint16_t kRqm = 0x8000, kDrs = 0x1000, kDrc = 0x0400;

    void setup(Model model, Map map, double hz);
    // SR as read: DRS only shows in 16-bit mode.
    uint16_t srValue() const { return sr_ & kDrc ? static_cast<uint16_t>(sr_ & ~kDrs) : sr_; }
    void op(uint32_t opcode);
    void jump(uint32_t opcode);
    void ld(uint16_t value, int dst);
    void push(uint16_t v) {
        stack_[sp_ & stackMask_] = v;
        sp_ = static_cast<uint8_t>((sp_ + 1) & stackMask_);
    }
    uint16_t pop() {
        sp_ = static_cast<uint8_t>((sp_ - 1) & stackMask_);
        return stack_[sp_ & stackMask_];
    }

    template <class Ar>
    void state(Ar& ar) {
        ar(pc_, rp_, dp_, sp_, a_, b_, tr_, trb_, k_, l_, m_, n_, dr_, sr_, so_, si_, budget_);
        flagA_.state(ar);
        flagB_.state(ar);
        for (uint16_t& s : stack_) ar(s);
        for (uint16_t& d : dataRam_) ar(d);
    }

    Model model_ = Model::Upd7725;
    Map map_ = Map::LoRom;
    std::vector<uint32_t> program_;
    std::vector<uint16_t> dataRom_;
    std::array<uint16_t, 2048> dataRam_{};
    std::array<uint16_t, 16> stack_{};
    uint16_t pcMask_ = 0x7FF, rpMask_ = 0x3FF, dpMask_ = 0xFF;
    uint8_t stackMask_ = 3;

    uint16_t pc_ = 0, rp_ = 0, dp_ = 0;
    uint8_t sp_ = 0;
    uint16_t a_ = 0, b_ = 0, tr_ = 0, trb_ = 0, k_ = 0, l_ = 0, m_ = 0, n_ = 0;
    uint16_t dr_ = 0, sr_ = 0, so_ = 0, si_ = 0;
    Flags flagA_, flagB_;

    int64_t hz_ = 7600000, masterHz_ = 21477272;
    int64_t budget_ = 0;
};

}  // namespace plumnin::snes
