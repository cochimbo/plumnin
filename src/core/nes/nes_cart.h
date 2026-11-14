#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../state_io.h"

namespace plumnin::nes {

enum class Mirroring : uint8_t { Horizontal, Vertical, SingleA, SingleB, FourScreen };

// What the cartridge's header says.
struct CartInfo {
    int mapper = 0, submapper = 0;
    Mirroring mirroring = Mirroring::Horizontal;
    bool battery = false;
    bool pal = false;
    size_t prgRamSize = 0x2000;
    size_t chrRamSize = 0;  // 0: the cartridge has CHR ROM
};

// The cartridge: PRG ROM, CHR ROM or RAM, PRG RAM, and the mapper chip that banks
// them, picks the nametable mirroring and sometimes counts scanlines for an IRQ.
// Banks are mapped in 8 KB pages of PRG and 1 KB pages of CHR; each mapper only
// says where its pages point.
class Mapper {
public:
    virtual ~Mapper() = default;

    // The cartridge from an iNES / NES 2.0 file, or null with the reason in Spanish.
    static std::unique_ptr<Mapper> create(const std::vector<uint8_t>& file, std::string* error);

    virtual void reset();  // power on: registers to their start

    // The processor's side, 4020-FFFF. `openBus` is what nothing driving the bus gives.
    virtual uint8_t cpuRead(uint16_t address, uint8_t openBus);
    virtual void cpuWrite(uint16_t address, uint8_t value);
    // The picture processor's side, 0000-1FFF (pattern tables). Each address it puts on
    // its bus is also told to the mapper (ppuAddress), for the chips that watch A12.
    uint8_t chrRead(uint16_t address) const { return chr_[chrMap_[(address >> 10) & 7] + (address & 0x3FF)]; }
    void chrWrite(uint16_t address, uint8_t value) {
        if (chrIsRam_) chr_[chrMap_[(address >> 10) & 7] + (address & 0x3FF)] = value;
    }
    virtual void ppuAddress(uint16_t /*address*/, uint64_t /*dot*/) {}
    virtual bool watchesPpu() const { return false; }
    // Nametable page (0-3, 2 and 3 only with four-screen VRAM) of 2000-2FFF.
    int nametable(uint16_t address) const { return ntMap_[(address >> 10) & 3]; }
    // One processor cycle, for the mappers that count them.
    virtual void cpuClock() {}
    virtual bool irq() const { return false; }

    const CartInfo& info() const { return info_; }
    bool hasBattery() const { return info_.battery && !prgRam_.empty(); }
    std::vector<uint8_t>& prgRam() { return prgRam_; }
    bool prgRamChanged() const { return ramChanged_; }
    void clearPrgRamChanged() { ramChanged_ = false; }
    // The PRG ROM as banked now at 8000-FFFF (for the achievements' map and cheats).
    uint8_t prgByte(uint16_t address) const { return prg_[prgMap_[(address >> 13) & 3] + (address & 0x1FFF)]; }

    // The PRG banks mapped now (not the mapper's registers that chose them).
    struct Banks {
        std::array<uint32_t, 4> prg{};
        uint32_t ramOffset = 0;
    };
    Banks banks() const { return {prgMap_, ramOffset_}; }
    void setBanks(const Banks& b) { prgMap_ = b.prg, ramOffset_ = b.ramOffset; }
    // Which 8 KB bank of the PRG ROM is at `address` (8000-FFFF).
    uint32_t prgBank(uint16_t address) const { return prgMap_[(address >> 13) & 3] / 0x2000; }

    void save(StateWriter& w) { io(w); saveExtra(w); }
    void load(StateReader& r) {
        io(r);
        loadExtra(r);
    }

protected:
    template <class Ar>
    void io(Ar& ar) {
        ar(prgMap_, chrMap_, ntMap_, prgRam_, ramEnabled_, ramWritable_, ramOffset_);
        if (chrIsRam_) ar(chr_);
    }
    virtual void saveExtra(StateWriter&) {}
    virtual void loadExtra(StateReader&) {}

    // Banks: a negative number counts from the end (-1 the last bank).
    void setPrg8(int slot, int bank);
    void setPrg16(int slot, int bank) { setPrg8(slot * 2, bank * 2), setPrg8(slot * 2 + 1, bank * 2 + 1); }
    void setPrg32(int bank) { setPrg16(0, bank * 2), setPrg16(1, bank * 2 + 1); }
    void setChr1(int slot, int bank);
    void setChr2(int slot, int bank) { setChr1(slot * 2, bank * 2), setChr1(slot * 2 + 1, bank * 2 + 1); }
    void setChr4(int slot, int bank) { setChr2(slot * 2, bank * 2), setChr2(slot * 2 + 1, bank * 2 + 1); }
    void setChr8(int bank) { setChr4(0, bank * 2), setChr4(1, bank * 2 + 1); }
    void setMirroring(Mirroring m);
    int prgBanks8() const { return static_cast<int>(prg_.size() / 0x2000); }
    int chrBanks1() const { return static_cast<int>(chr_.size() / 0x400); }

    CartInfo info_;
    std::vector<uint8_t> prg_, chr_, prgRam_;
    bool chrIsRam_ = false;
    std::array<uint32_t, 4> prgMap_{};
    std::array<uint32_t, 8> chrMap_{};
    std::array<uint8_t, 4> ntMap_{};
    bool ramEnabled_ = true, ramWritable_ = true;
    uint32_t ramOffset_ = 0;  // the PRG RAM bank at 6000
    bool ramChanged_ = false;
};

// A mapper's own registers in its savestate: NES_MAPPER_STATE(reg1, reg2, ...).
#define NES_MAPPER_STATE(...)                                     \
    void saveExtra(StateWriter& w) override { w(__VA_ARGS__); } \
    void loadExtra(StateReader& r) override { r(__VA_ARGS__); }

}  // namespace plumnin::nes
