#pragma once

#include <cstdint>

#include "../state_io.h"

namespace plumnin::snes {

// What the processor is wired to: every cycle is a read, a write or an idle cycle, and
// the bus decides how long each one takes (the SNES's memory is 6, 8 or 12 master
// clocks a cycle depending on where it is).
class CpuBus {
public:
    virtual ~CpuBus() = default;
    virtual uint8_t read(uint32_t address) = 0;
    virtual void write(uint32_t address, uint8_t value) = 0;
    virtual void idle() = 0;
    // Where an NMI or IRQ goes, if the bus gives it instead of the vector in memory
    // (the SA-1 has its vectors in registers).
    virtual bool hardwareVector(bool /*nmi*/, uint16_t& /*pc*/) { return false; }
};

// The 65C816 (the 5A22's core): the 6502's sixteen-bit successor with its emulation
// mode, one bus cycle at a time as the real one makes them, so that the cycle counts
// and the order of the accesses are right (SingleStepTests checks both).
class Cpu {
public:
    void setBus(CpuBus* bus) { bus_ = bus; }
    void power();
    void reset();
    // One instruction, or the interrupt that is due.
    void step();

    // The interrupt lines: NMI an edge the console raises, IRQ a level it holds.
    void raiseNmi() { nmiPending_ = true; }
    void setIrq(bool on) { irqLine_ = on; }
    bool waiting() const { return waiting_; }
    bool stopped() const { return stopped_; }
    void clearStop() { stopped_ = false; }

    struct Regs {
        uint16_t a = 0, x = 0, y = 0, s = 0x01FF, d = 0, pc = 0;
        uint8_t db = 0, pb = 0, p = 0x34;
        bool e = true;
    };
    Regs r;

    // The flags.
    enum : uint8_t { C = 0x01, Z = 0x02, I = 0x04, D = 0x08, X = 0x10, M = 0x20, V = 0x40, N = 0x80 };

    template <class Ar>
    void state(Ar& ar) {
        ar(r.a, r.x, r.y, r.s, r.d, r.pc, r.db, r.pb, r.p, r.e, nmiPending_, irqLine_, waiting_, stopped_);
    }

private:
    // An effective address: the next byte is at address + 1, which wraps in bank 0 for
    // the direct page and the stack, and carries into the next bank otherwise.
    struct Ea {
        uint32_t a;
        bool wrap16;
        uint32_t at(int i) const {
            return wrap16 ? static_cast<uint16_t>(a + static_cast<uint32_t>(i)) : (a + static_cast<uint32_t>(i)) & 0xFFFFFF;
        }
    };
    enum class Mode : uint8_t { Imm, Dp, DpX, DpY, Abs, AbsX, AbsY, Long, LongX, Ind, IndX, IndY, IndL, IndLY, Sr, SrY };

    uint8_t read(uint32_t a) { return bus_->read(a & 0xFFFFFF); }
    void write(uint32_t a, uint8_t v) { bus_->write(a & 0xFFFFFF, v); }
    void idle() { bus_->idle(); }
    uint8_t fetch() {
        const uint8_t v = read(static_cast<uint32_t>(r.pb) << 16 | r.pc);
        ++r.pc;
        return v;
    }
    uint16_t fetch16() {
        const uint8_t lo = fetch();
        return static_cast<uint16_t>(lo | fetch() << 8);
    }
    bool m8() const { return r.p & M; }
    bool x8() const { return r.p & X; }
    // The direct page: in emulation mode with D's low byte 0 it wraps within its page.
    uint8_t readDirect(uint32_t offset) {
        if (r.e && !(r.d & 0xFF)) return read((r.d & 0xFF00) | (offset & 0xFF));
        return read(static_cast<uint16_t>(r.d + offset));
    }
    uint8_t readDirectN(uint32_t offset) { return read(static_cast<uint16_t>(r.d + offset)); }
    void idleDirect() {
        if (r.d & 0xFF) idle();
    }
    // The stack: on page 1 in emulation mode. The N versions are the 65C816's own
    // instructions, which run past page 1 and fix S afterwards.
    void push(uint8_t v) {
        write(r.s, v);
        if (r.e) r.s = static_cast<uint16_t>(0x100 | ((r.s - 1) & 0xFF));
        else --r.s;
    }
    uint8_t pull() {
        if (r.e) r.s = static_cast<uint16_t>(0x100 | ((r.s + 1) & 0xFF));
        else ++r.s;
        return read(r.s);
    }
    void pushN(uint8_t v) { write(r.s--, v); }
    uint8_t pullN() { return read(++r.s); }
    void fixStack() {
        if (r.e) r.s = static_cast<uint16_t>(0x100 | (r.s & 0xFF));
    }
    void setP(uint8_t p);

    Ea address(Mode m, bool write);
    uint16_t load(Mode m, bool wide);
    void store(Mode m, uint16_t v, bool wide);
    template <class F>
    void modify(Mode m, F&& f);
    void setNZ(uint16_t v, bool wide) {
        r.p = static_cast<uint8_t>(r.p & ~(N | Z));
        if (wide) r.p |= (v ? 0 : Z) | (v & 0x8000 ? N : 0);
        else r.p |= (v & 0xFF ? 0 : Z) | (v & 0x80 ? N : 0);
    }
    void flag(uint8_t f, bool on) { r.p = static_cast<uint8_t>(on ? r.p | f : r.p & ~f); }
    void setA(uint16_t v) { r.a = m8() ? static_cast<uint16_t>((r.a & 0xFF00) | (v & 0xFF)) : v; }
    void setIndex(uint16_t& reg, uint16_t v) { reg = x8() ? static_cast<uint16_t>(v & 0xFF) : v; }

    void adc(uint16_t v);
    void sbc(uint16_t v);
    void compare(uint16_t reg, uint16_t v, bool wide);
    void branch(bool taken);
    void interrupt(uint16_t nativeVector, uint16_t emulationVector, bool software);
    void blockMove(int step);
    void execute(uint8_t op);

    CpuBus* bus_ = nullptr;
    bool nmiPending_ = false, irqLine_ = false, waiting_ = false, stopped_ = false;
};

}  // namespace plumnin::snes
