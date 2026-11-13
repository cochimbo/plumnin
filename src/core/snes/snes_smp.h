#pragma once

#include <cstdint>

#include "../state_io.h"

namespace plumnin::snes {

// What the sound processor is wired to: its 64 KB, its registers at F0-FF and the IPL
// ROM. An idle cycle is a cycle without a read or a write that matters.
class SmpBus {
public:
    virtual ~SmpBus() = default;
    virtual uint8_t read(uint16_t address) = 0;
    virtual void write(uint16_t address, uint8_t value) = 0;
    virtual void idle() = 0;
};

// The SPC700, the sound module's processor, one bus cycle at a time.
class Smp {
public:
    void setBus(SmpBus* bus) { bus_ = bus; }
    void power();
    // One instruction.
    void step();
    bool halted() const { return halted_; }

    struct Regs {
        uint8_t a = 0, x = 0, y = 0, sp = 0xEF, p = 0x02;
        uint16_t pc = 0xFFC0;
    };
    Regs r;

    enum : uint8_t { C = 0x01, Z = 0x02, I = 0x04, H = 0x08, B = 0x10, P = 0x20, V = 0x40, N = 0x80 };

    template <class Ar>
    void state(Ar& ar) {
        ar(r.a, r.x, r.y, r.sp, r.p, r.pc, halted_);
    }

private:
    uint8_t read(uint16_t a) { return bus_->read(a); }
    void write(uint16_t a, uint8_t v) { bus_->write(a, v); }
    void idle() { bus_->idle(); }
    uint8_t fetch() { return read(r.pc++); }
    uint16_t fetch16() {
        const uint8_t lo = fetch();
        return static_cast<uint16_t>(lo | fetch() << 8);
    }
    uint16_t page() const { return r.p & P ? 0x100 : 0; }
    uint8_t load(uint8_t a) { return read(static_cast<uint16_t>(page() | a)); }
    void store(uint8_t a, uint8_t v) { write(static_cast<uint16_t>(page() | a), v); }
    void push(uint8_t v) { write(static_cast<uint16_t>(0x100 | r.sp--), v); }
    uint8_t pull() { return read(static_cast<uint16_t>(0x100 | ++r.sp)); }
    void flag(uint8_t f, bool on) { r.p = static_cast<uint8_t>(on ? r.p | f : r.p & ~f); }
    uint8_t zn(uint8_t v) {
        flag(Z, !v);
        flag(N, v & 0x80);
        return v;
    }
    uint16_t ya() const { return static_cast<uint16_t>(r.y << 8 | r.a); }
    void setYa(uint16_t v) {
        r.a = static_cast<uint8_t>(v);
        r.y = static_cast<uint8_t>(v >> 8);
    }

    // The ALU: what the instructions with the same operation share.
    enum class Op : uint8_t { Or, And, Eor, Cmp, Adc, Sbc, Mov, Asl, Rol, Lsr, Ror, Dec, Inc };
    uint8_t alu(Op op, uint8_t x, uint8_t y);
    uint8_t adc(uint8_t x, uint8_t y);
    void branch(bool take);
    void execute(uint8_t op);

    SmpBus* bus_ = nullptr;
    bool halted_ = false;
};

}  // namespace plumnin::snes
