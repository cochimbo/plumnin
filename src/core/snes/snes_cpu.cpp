// The 65C816. Each instruction makes the bus cycles the real processor makes, in the
// same order: the idle cycles of the direct page when D's low byte is not 0, of an
// index that crosses a page (always for writes and for sixteen-bit indexes), the extra
// write of read-modify-write instructions in emulation mode...
#include "snes_cpu.h"

namespace plumnin::snes {

void Cpu::power() {
    r = Regs{};
    nmiPending_ = irqLine_ = waiting_ = stopped_ = false;
    reset();
}

void Cpu::reset() {
    // The reset puts the processor in emulation mode with the stack on page 1 and reads
    // its vector at FFFC (the SNES's reset makes a few bus cycles first, as it does).
    r.e = true;
    r.p = static_cast<uint8_t>((r.p | M | X | I) & ~D);
    r.x &= 0xFF;
    r.y &= 0xFF;
    r.s = static_cast<uint16_t>(0x100 | (r.s & 0xFF));
    r.d = 0;
    r.db = 0;
    r.pb = 0;
    waiting_ = stopped_ = false;
    nmiPending_ = false;
    for (int i = 0; i < 2; ++i) idle();
    for (int i = 0; i < 3; ++i) {
        read(0x100 | (r.s & 0xFF));
        r.s = static_cast<uint16_t>(0x100 | ((r.s - 1) & 0xFF));
    }
    const uint8_t lo = read(0xFFFC);
    r.pc = static_cast<uint16_t>(lo | read(0xFFFD) << 8);
}

void Cpu::setP(uint8_t p) {
    r.p = p;
    if (r.e) r.p |= M | X;
    if (r.p & X) {
        r.x &= 0xFF;
        r.y &= 0xFF;
    }
}

Cpu::Ea Cpu::address(Mode m, bool forWrite) {
    switch (m) {
        case Mode::Dp: {
            const uint8_t u = fetch();
            idleDirect();
            return {static_cast<uint16_t>(r.d + u), true};
        }
        case Mode::DpX:
        case Mode::DpY: {
            const uint8_t u = fetch();
            idleDirect();
            idle();
            const uint16_t index = m == Mode::DpX ? r.x : r.y;
            if (r.e && !(r.d & 0xFF)) return {static_cast<uint32_t>((r.d & 0xFF00) | ((u + index) & 0xFF)), true};
            return {static_cast<uint16_t>(r.d + u + index), true};
        }
        case Mode::Abs: return {static_cast<uint32_t>(r.db) << 16 | fetch16(), false};
        case Mode::AbsX:
        case Mode::AbsY: {
            const uint32_t base = static_cast<uint32_t>(r.db) << 16 | fetch16();
            const uint32_t to = base + (m == Mode::AbsX ? r.x : r.y);
            if (forWrite || !x8() || ((base ^ to) & 0xFF00)) idle();
            return {to & 0xFFFFFF, false};
        }
        case Mode::Long:
        case Mode::LongX: {
            uint32_t a = fetch16();
            a |= static_cast<uint32_t>(fetch()) << 16;
            if (m == Mode::LongX) a += r.x;
            return {a & 0xFFFFFF, false};
        }
        case Mode::Ind:
        case Mode::IndX:
        case Mode::IndY: {
            const uint8_t u = fetch();
            idleDirect();
            uint32_t offset = u;
            if (m == Mode::IndX) {
                idle();
                offset += r.x;
            }
            const uint8_t lo = readDirect(offset);
            const uint8_t hi = readDirect(offset + 1);
            const uint32_t base = static_cast<uint32_t>(r.db) << 16 | static_cast<uint32_t>(hi << 8 | lo);
            if (m != Mode::IndY) return {base, false};
            const uint32_t to = base + r.y;
            if (forWrite || !x8() || ((base ^ to) & 0xFF00)) idle();
            return {to & 0xFFFFFF, false};
        }
        case Mode::IndL:
        case Mode::IndLY: {
            const uint8_t u = fetch();
            idleDirect();
            uint32_t a = readDirectN(u);
            a |= static_cast<uint32_t>(readDirectN(u + 1u)) << 8;
            a |= static_cast<uint32_t>(readDirectN(u + 2u)) << 16;
            if (m == Mode::IndLY) a += r.y;
            return {a & 0xFFFFFF, false};
        }
        case Mode::Sr: {
            const uint8_t u = fetch();
            idle();
            return {static_cast<uint16_t>(r.s + u), true};
        }
        case Mode::SrY: {
            const uint8_t u = fetch();
            idle();
            const uint8_t lo = read(static_cast<uint16_t>(r.s + u));
            const uint8_t hi = read(static_cast<uint16_t>(r.s + u + 1));
            idle();
            const uint32_t base = static_cast<uint32_t>(r.db) << 16 | static_cast<uint32_t>(hi << 8 | lo);
            return {(base + r.y) & 0xFFFFFF, false};
        }
        case Mode::Imm: break;
    }
    return {0, false};
}

uint16_t Cpu::load(Mode m, bool wide) {
    if (m == Mode::Imm) return wide ? fetch16() : fetch();
    const Ea e = address(m, false);
    const uint8_t lo = read(e.at(0));
    if (!wide) return lo;
    return static_cast<uint16_t>(lo | read(e.at(1)) << 8);
}

void Cpu::store(Mode m, uint16_t v, bool wide) {
    const Ea e = address(m, true);
    write(e.at(0), static_cast<uint8_t>(v));
    if (wide) write(e.at(1), static_cast<uint8_t>(v >> 8));
}

template <class F>
void Cpu::modify(Mode m, F&& change) {
    const Ea e = address(m, true);
    const bool wide = !m8();
    uint16_t v = read(e.at(0));
    if (wide) v = static_cast<uint16_t>(v | read(e.at(1)) << 8);
    // The cycle while it works: a write of the old value in emulation mode, as the 6502.
    if (r.e) write(e.at(0), static_cast<uint8_t>(v));
    else idle();
    v = change(v, wide);
    if (wide) write(e.at(1), static_cast<uint8_t>(v >> 8));
    write(e.at(0), static_cast<uint8_t>(v));
}

void Cpu::adc(uint16_t v) {
    int result;
    const bool carry = r.p & C;
    if (m8()) {
        const int a = r.a & 0xFF, d = v & 0xFF;
        if (!(r.p & D)) {
            result = a + d + carry;
        } else {
            result = (a & 0x0F) + (d & 0x0F) + carry;
            if (result > 0x09) result += 0x06;
            const int c = result > 0x0F;
            result = (a & 0xF0) + (d & 0xF0) + (c << 4) + (result & 0x0F);
        }
        flag(V, ~(a ^ d) & (a ^ result) & 0x80);
        if ((r.p & D) && result > 0x9F) result += 0x60;
        flag(C, result > 0xFF);
        setA(static_cast<uint16_t>(result));
        setNZ(static_cast<uint16_t>(result), false);
    } else {
        const int a = r.a, d = v;
        if (!(r.p & D)) {
            result = a + d + carry;
        } else {
            result = (a & 0x000F) + (d & 0x000F) + carry;
            if (result > 0x0009) result += 0x0006;
            int c = result > 0x000F;
            result = (a & 0x00F0) + (d & 0x00F0) + (c << 4) + (result & 0x000F);
            if (result > 0x009F) result += 0x0060;
            c = result > 0x00FF;
            result = (a & 0x0F00) + (d & 0x0F00) + (c << 8) + (result & 0x00FF);
            if (result > 0x09FF) result += 0x0600;
            c = result > 0x0FFF;
            result = (a & 0xF000) + (d & 0xF000) + (c << 12) + (result & 0x0FFF);
        }
        flag(V, ~(a ^ d) & (a ^ result) & 0x8000);
        if ((r.p & D) && result > 0x9FFF) result += 0x6000;
        flag(C, result > 0xFFFF);
        r.a = static_cast<uint16_t>(result);
        setNZ(r.a, true);
    }
}

void Cpu::sbc(uint16_t v) {
    int result;
    const bool carry = r.p & C;
    if (m8()) {
        const int a = r.a & 0xFF, d = ~v & 0xFF;
        if (!(r.p & D)) {
            result = a + d + carry;
        } else {
            result = (a & 0x0F) + (d & 0x0F) + carry;
            if (result <= 0x0F) result -= 0x06;
            const int c = result > 0x0F;
            result = (a & 0xF0) + (d & 0xF0) + (c << 4) + (result & 0x0F);
        }
        flag(V, ~(a ^ d) & (a ^ result) & 0x80);
        if ((r.p & D) && result <= 0xFF) result -= 0x60;
        flag(C, result > 0xFF);
        setA(static_cast<uint16_t>(result));
        setNZ(static_cast<uint16_t>(result), false);
    } else {
        const int a = r.a, d = ~v & 0xFFFF;
        if (!(r.p & D)) {
            result = a + d + carry;
        } else {
            result = (a & 0x000F) + (d & 0x000F) + carry;
            if (result <= 0x000F) result -= 0x0006;
            int c = result > 0x000F;
            result = (a & 0x00F0) + (d & 0x00F0) + (c << 4) + (result & 0x000F);
            if (result <= 0x00FF) result -= 0x0060;
            c = result > 0x00FF;
            result = (a & 0x0F00) + (d & 0x0F00) + (c << 8) + (result & 0x00FF);
            if (result <= 0x0FFF) result -= 0x0600;
            c = result > 0x0FFF;
            result = (a & 0xF000) + (d & 0xF000) + (c << 12) + (result & 0x0FFF);
        }
        flag(V, ~(a ^ d) & (a ^ result) & 0x8000);
        if ((r.p & D) && result <= 0xFFFF) result -= 0x6000;
        flag(C, result > 0xFFFF);
        r.a = static_cast<uint16_t>(result);
        setNZ(r.a, true);
    }
}

void Cpu::compare(uint16_t reg, uint16_t v, bool wide) {
    if (!wide) {
        reg &= 0xFF;
        v &= 0xFF;
    }
    flag(C, reg >= v);
    setNZ(static_cast<uint16_t>(reg - v), wide);
}

void Cpu::branch(bool taken) {
    const int8_t offset = static_cast<int8_t>(fetch());
    if (!taken) return;
    idle();
    const uint16_t to = static_cast<uint16_t>(r.pc + offset);
    if (r.e && ((to ^ r.pc) & 0xFF00)) idle();
    r.pc = to;
}

void Cpu::interrupt(uint16_t nativeVector, uint16_t emulationVector, bool software) {
    if (software) {
        fetch();  // the signature byte
    } else {
        read(static_cast<uint32_t>(r.pb) << 16 | r.pc);
        idle();
    }
    if (!r.e) push(r.pb);
    push(static_cast<uint8_t>(r.pc >> 8));
    push(static_cast<uint8_t>(r.pc));
    // In emulation mode the pushed B flag (where X is) tells BRK from an IRQ.
    push(r.e && !software ? static_cast<uint8_t>(r.p & ~0x10) : r.p);
    r.p = static_cast<uint8_t>((r.p | I) & ~D);
    r.pb = 0;
    uint16_t to = 0;
    if (!software && bus_->hardwareVector(nativeVector == 0xFFEA, to)) {
        r.pc = to;
        return;
    }
    const uint16_t vector = r.e ? emulationVector : nativeVector;
    const uint8_t lo = read(vector);
    r.pc = static_cast<uint16_t>(lo | read(vector + 1u) << 8);
}

void Cpu::blockMove(int step) {
    const uint8_t to = fetch(), from = fetch();
    r.db = to;
    const uint8_t v = read(static_cast<uint32_t>(from) << 16 | r.x);
    write(static_cast<uint32_t>(to) << 16 | r.y, v);
    idle();
    setIndex(r.x, static_cast<uint16_t>(r.x + step));
    setIndex(r.y, static_cast<uint16_t>(r.y + step));
    idle();
    if (r.a--) r.pc = static_cast<uint16_t>(r.pc - 3);
}

void Cpu::step() {
    if (stopped_) {
        idle();
        return;
    }
    if (waiting_) {
        if (!nmiPending_ && !irqLine_) {
            idle();
            return;
        }
        waiting_ = false;
        idle();
    }
    if (nmiPending_) {
        nmiPending_ = false;
        interrupt(0xFFEA, 0xFFFA, false);
        return;
    }
    if (irqLine_ && !(r.p & I)) {
        interrupt(0xFFEE, 0xFFFE, false);
        return;
    }
    execute(fetch());
}

void Cpu::execute(uint8_t op) {
    // The eight ALU instructions share their fifteen addressing modes.
    static const Mode kAluModes[32] = {
        Mode::Imm, Mode::IndX, Mode::Imm, Mode::Sr,  Mode::Imm,  Mode::Dp,   Mode::Imm, Mode::IndL,
        Mode::Imm, Mode::Imm,  Mode::Imm, Mode::Imm, Mode::Imm,  Mode::Abs,  Mode::Imm, Mode::Long,
        Mode::Imm, Mode::IndY, Mode::Ind, Mode::SrY, Mode::Imm,  Mode::DpX,  Mode::Imm, Mode::IndLY,
        Mode::Imm, Mode::AbsY, Mode::Imm, Mode::Imm, Mode::Imm,  Mode::AbsX, Mode::Imm, Mode::LongX,
    };
    const unsigned low = op & 0x1F;
    const bool alu = (low & 1) ? low != 0x0B && low != 0x1B : low == 0x12;
    if (alu && op != 0x89) {
        const Mode m = kAluModes[low];
        const bool wide = !m8();
        switch (op >> 5) {
            case 0: setA(static_cast<uint16_t>(r.a | load(m, wide))), setNZ(r.a, wide); return;
            case 1: setA(static_cast<uint16_t>(r.a & load(m, wide))), setNZ(r.a, wide); return;
            case 2: setA(static_cast<uint16_t>(r.a ^ load(m, wide))), setNZ(r.a, wide); return;
            case 3: adc(load(m, wide)); return;
            case 4: store(m, r.a, wide); return;
            case 5: setA(load(m, wide)), setNZ(r.a, wide); return;
            case 6: compare(r.a, load(m, wide), wide); return;
            default: sbc(load(m, wide)); return;
        }
    }

    const bool wideM = !m8(), wideX = !x8();
    auto asl = [this](uint16_t v, bool wide) {
        flag(C, v & (wide ? 0x8000 : 0x80));
        v = static_cast<uint16_t>(v << 1);
        setNZ(v, wide);
        return v;
    };
    auto lsr = [this](uint16_t v, bool wide) {
        flag(C, v & 1);
        v = static_cast<uint16_t>((wide ? v : v & 0xFF) >> 1);
        setNZ(v, wide);
        return v;
    };
    auto rol = [this](uint16_t v, bool wide) {
        const bool c = r.p & C;
        flag(C, v & (wide ? 0x8000 : 0x80));
        v = static_cast<uint16_t>(v << 1 | c);
        setNZ(v, wide);
        return v;
    };
    auto ror = [this](uint16_t v, bool wide) {
        const bool c = r.p & C;
        flag(C, v & 1);
        if (!wide) v &= 0xFF;
        v = static_cast<uint16_t>(v >> 1 | (c ? (wide ? 0x8000 : 0x80) : 0));
        setNZ(v, wide);
        return v;
    };
    auto inc = [this](uint16_t v, bool wide) {
        v = static_cast<uint16_t>(v + 1);
        setNZ(v, wide);
        return v;
    };
    auto dec = [this](uint16_t v, bool wide) {
        v = static_cast<uint16_t>(v - 1);
        setNZ(v, wide);
        return v;
    };
    auto tsb = [this](uint16_t v, bool wide) {
        flag(Z, !(v & r.a & (wide ? 0xFFFF : 0xFF)));
        return static_cast<uint16_t>(v | r.a);
    };
    auto trb = [this](uint16_t v, bool wide) {
        flag(Z, !(v & r.a & (wide ? 0xFFFF : 0xFF)));
        return static_cast<uint16_t>(v & ~r.a);
    };
    auto accumulator = [&](auto&& change) {
        idle();
        setA(change(r.a, wideM));
    };
    auto bit = [&](Mode m) {
        const uint16_t v = load(m, wideM);
        const uint16_t masked = static_cast<uint16_t>(v & r.a & (wideM ? 0xFFFF : 0xFF));
        flag(Z, !masked);
        if (m != Mode::Imm) {
            flag(N, v & (wideM ? 0x8000 : 0x80));
            flag(V, v & (wideM ? 0x4000 : 0x40));
        }
    };
    auto loadIndex = [&](uint16_t& reg, Mode m) {
        setIndex(reg, load(m, wideX));
        setNZ(reg, wideX);
    };
    auto implied = [&](auto&& f) {
        idle();
        f();
    };
    auto transferIndex = [&](uint16_t& to, uint16_t from) {
        idle();
        setIndex(to, from);
        setNZ(to, wideX);
    };
    auto pushWide = [&](uint16_t v, bool wide) {
        idle();
        if (wide) push(static_cast<uint8_t>(v >> 8));
        push(static_cast<uint8_t>(v));
    };
    auto pullWide = [&](bool wide) {
        idle();
        idle();
        uint16_t v = pull();
        if (wide) v = static_cast<uint16_t>(v | pull() << 8);
        setNZ(v, wide);
        return v;
    };

    switch (op) {
        case 0x00: interrupt(0xFFE6, 0xFFFE, true); break;
        case 0x02: interrupt(0xFFE4, 0xFFF4, true); break;
        case 0x04: modify(Mode::Dp, tsb); break;
        case 0x0C: modify(Mode::Abs, tsb); break;
        case 0x14: modify(Mode::Dp, trb); break;
        case 0x1C: modify(Mode::Abs, trb); break;
        case 0x06: modify(Mode::Dp, asl); break;
        case 0x16: modify(Mode::DpX, asl); break;
        case 0x0E: modify(Mode::Abs, asl); break;
        case 0x1E: modify(Mode::AbsX, asl); break;
        case 0x26: modify(Mode::Dp, rol); break;
        case 0x36: modify(Mode::DpX, rol); break;
        case 0x2E: modify(Mode::Abs, rol); break;
        case 0x3E: modify(Mode::AbsX, rol); break;
        case 0x46: modify(Mode::Dp, lsr); break;
        case 0x56: modify(Mode::DpX, lsr); break;
        case 0x4E: modify(Mode::Abs, lsr); break;
        case 0x5E: modify(Mode::AbsX, lsr); break;
        case 0x66: modify(Mode::Dp, ror); break;
        case 0x76: modify(Mode::DpX, ror); break;
        case 0x6E: modify(Mode::Abs, ror); break;
        case 0x7E: modify(Mode::AbsX, ror); break;
        case 0xC6: modify(Mode::Dp, dec); break;
        case 0xD6: modify(Mode::DpX, dec); break;
        case 0xCE: modify(Mode::Abs, dec); break;
        case 0xDE: modify(Mode::AbsX, dec); break;
        case 0xE6: modify(Mode::Dp, inc); break;
        case 0xF6: modify(Mode::DpX, inc); break;
        case 0xEE: modify(Mode::Abs, inc); break;
        case 0xFE: modify(Mode::AbsX, inc); break;
        case 0x0A: accumulator(asl); break;
        case 0x2A: accumulator(rol); break;
        case 0x4A: accumulator(lsr); break;
        case 0x6A: accumulator(ror); break;
        case 0x1A: accumulator(inc); break;
        case 0x3A: accumulator(dec); break;

        case 0x24: bit(Mode::Dp); break;
        case 0x2C: bit(Mode::Abs); break;
        case 0x34: bit(Mode::DpX); break;
        case 0x3C: bit(Mode::AbsX); break;
        case 0x89: bit(Mode::Imm); break;

        case 0x64: store(Mode::Dp, 0, wideM); break;
        case 0x74: store(Mode::DpX, 0, wideM); break;
        case 0x9C: store(Mode::Abs, 0, wideM); break;
        case 0x9E: store(Mode::AbsX, 0, wideM); break;
        case 0x84: store(Mode::Dp, r.y, wideX); break;
        case 0x94: store(Mode::DpX, r.y, wideX); break;
        case 0x8C: store(Mode::Abs, r.y, wideX); break;
        case 0x86: store(Mode::Dp, r.x, wideX); break;
        case 0x96: store(Mode::DpY, r.x, wideX); break;
        case 0x8E: store(Mode::Abs, r.x, wideX); break;
        case 0xA0: loadIndex(r.y, Mode::Imm); break;
        case 0xA4: loadIndex(r.y, Mode::Dp); break;
        case 0xB4: loadIndex(r.y, Mode::DpX); break;
        case 0xAC: loadIndex(r.y, Mode::Abs); break;
        case 0xBC: loadIndex(r.y, Mode::AbsX); break;
        case 0xA2: loadIndex(r.x, Mode::Imm); break;
        case 0xA6: loadIndex(r.x, Mode::Dp); break;
        case 0xB6: loadIndex(r.x, Mode::DpY); break;
        case 0xAE: loadIndex(r.x, Mode::Abs); break;
        case 0xBE: loadIndex(r.x, Mode::AbsY); break;
        case 0xC0: compare(r.y, load(Mode::Imm, wideX), wideX); break;
        case 0xC4: compare(r.y, load(Mode::Dp, wideX), wideX); break;
        case 0xCC: compare(r.y, load(Mode::Abs, wideX), wideX); break;
        case 0xE0: compare(r.x, load(Mode::Imm, wideX), wideX); break;
        case 0xE4: compare(r.x, load(Mode::Dp, wideX), wideX); break;
        case 0xEC: compare(r.x, load(Mode::Abs, wideX), wideX); break;

        case 0x10: branch(!(r.p & N)); break;
        case 0x30: branch(r.p & N); break;
        case 0x50: branch(!(r.p & V)); break;
        case 0x70: branch(r.p & V); break;
        case 0x80: branch(true); break;
        case 0x90: branch(!(r.p & C)); break;
        case 0xB0: branch(r.p & C); break;
        case 0xD0: branch(!(r.p & Z)); break;
        case 0xF0: branch(r.p & Z); break;
        case 0x82: {
            const uint16_t offset = fetch16();
            idle();
            r.pc = static_cast<uint16_t>(r.pc + offset);
            break;
        }

        case 0x18: implied([&] { flag(C, false); }); break;
        case 0x38: implied([&] { flag(C, true); }); break;
        case 0x58: implied([&] { flag(I, false); }); break;
        case 0x78: implied([&] { flag(I, true); }); break;
        case 0xB8: implied([&] { flag(V, false); }); break;
        case 0xD8: implied([&] { flag(D, false); }); break;
        case 0xF8: implied([&] { flag(D, true); }); break;
        case 0xEA: idle(); break;
        case 0x42: idle(), ++r.pc; break;  // WDM: its operand is skipped, not read
        case 0xC2: {
            const uint8_t v = fetch();
            idle();
            setP(static_cast<uint8_t>(r.p & ~v));
            break;
        }
        case 0xE2: {
            const uint8_t v = fetch();
            idle();
            setP(static_cast<uint8_t>(r.p | v));
            break;
        }
        case 0xFB: {
            idle();
            const bool c = r.p & C;
            flag(C, r.e);
            r.e = c;
            if (r.e) {
                r.s = static_cast<uint16_t>(0x100 | (r.s & 0xFF));
                setP(r.p);
            }
            break;
        }

        case 0xC8: implied([&] { setIndex(r.y, static_cast<uint16_t>(r.y + 1)), setNZ(r.y, wideX); }); break;
        case 0x88: implied([&] { setIndex(r.y, static_cast<uint16_t>(r.y - 1)), setNZ(r.y, wideX); }); break;
        case 0xE8: implied([&] { setIndex(r.x, static_cast<uint16_t>(r.x + 1)), setNZ(r.x, wideX); }); break;
        case 0xCA: implied([&] { setIndex(r.x, static_cast<uint16_t>(r.x - 1)), setNZ(r.x, wideX); }); break;
        case 0xAA: transferIndex(r.x, r.a); break;
        case 0xA8: transferIndex(r.y, r.a); break;
        case 0xBA: transferIndex(r.x, r.s); break;
        case 0x9B: transferIndex(r.y, r.x); break;
        case 0xBB: transferIndex(r.x, r.y); break;
        case 0x8A: implied([&] { setA(r.x), setNZ(r.a, wideM); }); break;
        case 0x98: implied([&] { setA(r.y), setNZ(r.a, wideM); }); break;
        case 0x9A: implied([&] { r.s = r.e ? static_cast<uint16_t>(0x100 | (r.x & 0xFF)) : r.x; }); break;
        case 0x1B: implied([&] { r.s = r.e ? static_cast<uint16_t>(0x100 | (r.a & 0xFF)) : r.a; }); break;
        case 0x3B: implied([&] { r.a = r.s, setNZ(r.a, true); }); break;
        case 0x5B: implied([&] { r.d = r.a, setNZ(r.d, true); }); break;
        case 0x7B: implied([&] { r.a = r.d, setNZ(r.a, true); }); break;
        case 0xEB:
            idle();
            idle();
            r.a = static_cast<uint16_t>(r.a >> 8 | r.a << 8);
            setNZ(r.a & 0xFF, false);
            break;

        case 0x48: pushWide(r.a, wideM); break;
        case 0xDA: pushWide(r.x, wideX); break;
        case 0x5A: pushWide(r.y, wideX); break;
        case 0x08: pushWide(r.p, false); break;
        case 0x8B: pushWide(r.db, false); break;
        case 0x4B: pushWide(r.pb, false); break;
        case 0x68: setA(pullWide(wideM)); break;
        case 0xFA: setIndex(r.x, pullWide(wideX)); break;
        case 0x7A: setIndex(r.y, pullWide(wideX)); break;
        case 0xAB:
            idle();
            idle();
            r.db = pullN();
            setNZ(r.db, false);
            fixStack();
            break;
        case 0x28:
            idle();
            idle();
            setP(pull());
            break;
        case 0x0B:
            idle();
            pushN(static_cast<uint8_t>(r.d >> 8));
            pushN(static_cast<uint8_t>(r.d));
            fixStack();
            break;
        case 0x2B: {
            idle();
            idle();
            const uint8_t lo = pullN();
            r.d = static_cast<uint16_t>(lo | pullN() << 8);
            setNZ(r.d, true);
            fixStack();
            break;
        }
        case 0xF4: {
            const uint16_t v = fetch16();
            pushN(static_cast<uint8_t>(v >> 8));
            pushN(static_cast<uint8_t>(v));
            fixStack();
            break;
        }
        case 0xD4: {
            const uint8_t u = fetch();
            idleDirect();
            const uint8_t lo = readDirectN(u);
            const uint8_t hi = readDirectN(u + 1u);
            pushN(hi);
            pushN(lo);
            fixStack();
            break;
        }
        case 0x62: {
            const uint16_t offset = fetch16();
            idle();
            const uint16_t v = static_cast<uint16_t>(r.pc + offset);
            pushN(static_cast<uint8_t>(v >> 8));
            pushN(static_cast<uint8_t>(v));
            fixStack();
            break;
        }

        case 0x4C: r.pc = fetch16(); break;
        case 0x5C: {
            const uint16_t to = fetch16();
            r.pb = fetch();
            r.pc = to;
            break;
        }
        case 0x6C: {
            const uint16_t at = fetch16();
            const uint8_t lo = read(at);
            r.pc = static_cast<uint16_t>(lo | read(static_cast<uint16_t>(at + 1)) << 8);
            break;
        }
        case 0x7C: {
            const uint16_t at = static_cast<uint16_t>(fetch16() + r.x);
            idle();
            const uint32_t bank = static_cast<uint32_t>(r.pb) << 16;
            const uint8_t lo = read(bank | at);
            r.pc = static_cast<uint16_t>(lo | read(bank | static_cast<uint16_t>(at + 1)) << 8);
            break;
        }
        case 0xDC: {
            const uint16_t at = fetch16();
            const uint8_t lo = read(at);
            const uint8_t hi = read(static_cast<uint16_t>(at + 1));
            r.pb = read(static_cast<uint16_t>(at + 2));
            r.pc = static_cast<uint16_t>(lo | hi << 8);
            break;
        }
        case 0x20: {
            const uint16_t to = fetch16();
            idle();
            --r.pc;
            push(static_cast<uint8_t>(r.pc >> 8));
            push(static_cast<uint8_t>(r.pc));
            r.pc = to;
            break;
        }
        case 0x22: {
            const uint16_t to = fetch16();
            pushN(r.pb);
            idle();
            const uint8_t bank = fetch();
            --r.pc;
            pushN(static_cast<uint8_t>(r.pc >> 8));
            pushN(static_cast<uint8_t>(r.pc));
            r.pc = to;
            r.pb = bank;
            fixStack();
            break;
        }
        case 0xFC: {
            const uint8_t lo = fetch();
            pushN(static_cast<uint8_t>(r.pc >> 8));
            pushN(static_cast<uint8_t>(r.pc));
            const uint8_t hi = fetch();
            idle();
            const uint16_t at = static_cast<uint16_t>((hi << 8 | lo) + r.x);
            const uint32_t bank = static_cast<uint32_t>(r.pb) << 16;
            const uint8_t to = read(bank | at);
            r.pc = static_cast<uint16_t>(to | read(bank | static_cast<uint16_t>(at + 1)) << 8);
            fixStack();
            break;
        }
        case 0x60: {
            idle();
            idle();
            const uint8_t lo = pull();
            const uint8_t hi = pull();
            idle();
            r.pc = static_cast<uint16_t>((lo | hi << 8) + 1);
            break;
        }
        case 0x6B: {
            idle();
            idle();
            const uint8_t lo = pullN();
            const uint8_t hi = pullN();
            r.pb = pullN();
            r.pc = static_cast<uint16_t>((lo | hi << 8) + 1);
            fixStack();
            break;
        }
        case 0x40: {
            idle();
            idle();
            setP(pull());
            const uint8_t lo = pull();
            const uint8_t hi = pull();
            r.pc = static_cast<uint16_t>(lo | hi << 8);
            if (!r.e) r.pb = pull();
            break;
        }
        case 0x44: blockMove(-1); break;
        case 0x54: blockMove(1); break;
        case 0xCB:
            idle();
            idle();
            waiting_ = true;
            break;
        case 0xDB:
            idle();
            idle();
            stopped_ = true;
            break;
        default: break;
    }
}

}  // namespace plumnin::snes
