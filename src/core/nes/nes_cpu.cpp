// The 2A03's 6502 (without decimal mode), with every unofficial instruction and the
// dummy reads and writes the real one makes, which registers with side effects see.
#include "nes.h"

namespace plumnin {

namespace {

using M = Nes::Mode;

}  // namespace

const Nes::Mode Nes::kModes[256] = {
    M::Imp, M::IndX, M::None, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Acc, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,
    M::Special, M::IndX, M::None, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Acc, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,
    M::Imp, M::IndX, M::None, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Acc, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,
    M::Imp, M::IndX, M::None, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Acc, M::Imm, M::Ind, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,
    M::Imm, M::IndX, M::Imm, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Imp, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndYW, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroY, M::ZeroY, M::Imp, M::AbsYW, M::Imp, M::AbsYW, M::AbsXW, M::AbsXW, M::AbsYW, M::AbsYW,
    M::Imm, M::IndX, M::Imm, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Imp, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndY, M::ZeroX, M::ZeroX, M::ZeroY, M::ZeroY, M::Imp, M::AbsY, M::Imp, M::AbsY, M::AbsX, M::AbsX, M::AbsY, M::AbsY,
    M::Imm, M::IndX, M::Imm, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Imp, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,
    M::Imm, M::IndX, M::Imm, M::IndX, M::Zero, M::Zero, M::Zero, M::Zero, M::Imp, M::Imm, M::Imp, M::Imm, M::Abs, M::Abs, M::Abs, M::Abs,
    M::Rel, M::IndY, M::None, M::IndYW, M::ZeroX, M::ZeroX, M::ZeroX, M::ZeroX, M::Imp, M::AbsY, M::Imp, M::AbsYW, M::AbsX, M::AbsX, M::AbsXW, M::AbsXW,};

uint16_t Nes::operand(Mode m) {
    switch (m) {
        case Mode::Imm:
        case Mode::Rel:
        case Mode::Zero: return fetch();
        case Mode::ZeroX:
        case Mode::ZeroY: {
            const uint8_t z = fetch();
            dummyRead(z);
            return static_cast<uint8_t>(z + (m == Mode::ZeroX ? x_ : y_));
        }
        case Mode::Ind:
        case Mode::Abs: return fetch16();
        case Mode::IndX: {
            uint8_t z = fetch();
            dummyRead(z);
            z = static_cast<uint8_t>(z + x_);
            const uint8_t lo = read(z);
            return static_cast<uint16_t>(lo | read(static_cast<uint8_t>(z + 1)) << 8);
        }
        case Mode::IndY:
        case Mode::IndYW:
        case Mode::AbsX:
        case Mode::AbsXW:
        case Mode::AbsY:
        case Mode::AbsYW: {
            if (m == Mode::IndY || m == Mode::IndYW) {
                const uint8_t z = fetch();
                const uint8_t lo = read(z);
                base_ = static_cast<uint16_t>(lo | read(static_cast<uint8_t>(z + 1)) << 8);
            } else {
                base_ = fetch16();
            }
            const uint8_t index = m == Mode::AbsX || m == Mode::AbsXW ? x_ : y_;
            const uint16_t address = static_cast<uint16_t>(base_ + index);
            const bool crossed = (base_ ^ address) & 0xFF00;
            // The first try at the address, before the carry reaches its high byte:
            // only when it is wrong, unless the instruction writes.
            if (crossed || m == Mode::IndYW || m == Mode::AbsXW || m == Mode::AbsYW)
                dummyRead(static_cast<uint16_t>((base_ & 0xFF00) | (address & 0xFF)));
            return address;
        }
        case Mode::Special: return 0;
        default: dummyRead(pc_); return 0;  // implied: the next byte is read and ignored
    }
}

void Nes::branch(uint16_t offset, bool taken) {
    if (!taken) return;
    // A taken branch that stays on its page does not look at the IRQ in its last
    // cycle: the next instruction runs first.
    if (runIrq_ && !prevRunIrq_) runIrq_ = false;
    dummyRead(pc_);
    const uint16_t target = static_cast<uint16_t>(pc_ + static_cast<int8_t>(offset));
    if ((target ^ pc_) & 0xFF00) dummyRead(static_cast<uint16_t>((pc_ & 0xFF00) | (target & 0xFF)));
    pc_ = target;
}

void Nes::brk() {
    const uint16_t ret = static_cast<uint16_t>(pc_ + 1);
    push(static_cast<uint8_t>(ret >> 8));
    push(static_cast<uint8_t>(ret));
    // An NMI arriving now takes the vector over, and the B flag is still pushed.
    const bool nmi = needNmi_;
    if (nmi) needNmi_ = false;
    push(static_cast<uint8_t>(p_ | B | U));
    p_ |= I;
    const uint16_t vector = nmi ? 0xFFFA : 0xFFFE;
    const uint8_t lo = read(vector);
    pc_ = static_cast<uint16_t>(lo | read(static_cast<uint16_t>(vector + 1)) << 8);
    if (tracer_) tracer_->call(pc_, static_cast<uint16_t>(0x100 | sp_), true);
    // The handler's first instruction runs before any NMI.
    prevNeedNmi_ = false;
}

void Nes::interrupt() {
    dummyRead(pc_);
    dummyRead(pc_);
    push(static_cast<uint8_t>(pc_ >> 8));
    push(static_cast<uint8_t>(pc_));
    const bool nmi = needNmi_;
    if (nmi) needNmi_ = false;
    push(static_cast<uint8_t>(p_ | U));
    p_ |= I;
    const uint16_t vector = nmi ? 0xFFFA : 0xFFFE;
    const uint8_t lo = read(vector);
    pc_ = static_cast<uint16_t>(lo | read(static_cast<uint16_t>(vector + 1)) << 8);
    if (tracer_) tracer_->call(pc_, static_cast<uint16_t>(0x100 | sp_), true);
}

void Nes::adc(uint8_t v) {
    const unsigned sum = a_ + v + (p_ & C);
    flag(V, ~(a_ ^ v) & (a_ ^ sum) & 0x80);
    flag(C, sum > 0xFF);
    a_ = static_cast<uint8_t>(sum);
    setZN(a_);
}

void Nes::compare(uint8_t reg, uint8_t v) {
    flag(C, reg >= v);
    setZN(static_cast<uint8_t>(reg - v));
}

uint8_t Nes::asl(uint8_t v) {
    flag(C, v & 0x80);
    v = static_cast<uint8_t>(v << 1);
    setZN(v);
    return v;
}

uint8_t Nes::lsr(uint8_t v) {
    flag(C, v & 1);
    v >>= 1;
    setZN(v);
    return v;
}

uint8_t Nes::rol(uint8_t v) {
    const bool carry = p_ & C;
    flag(C, v & 0x80);
    v = static_cast<uint8_t>(v << 1 | carry);
    setZN(v);
    return v;
}

uint8_t Nes::ror(uint8_t v) {
    const bool carry = p_ & C;
    flag(C, v & 1);
    v = static_cast<uint8_t>(v >> 1 | carry << 7);
    setZN(v);
    return v;
}

// SHA, SHX, SHY and TAS store a register ANDed with the address's high byte plus
// one; when indexing crossed a page, that value also becomes the high byte.
void Nes::storeHigh(uint8_t index, uint8_t reg) {
    const uint16_t address = static_cast<uint16_t>(base_ + index);
    const uint8_t v = static_cast<uint8_t>(reg & ((base_ >> 8) + 1));
    uint16_t target = address;
    if ((base_ ^ address) & 0xFF00) target = static_cast<uint16_t>((address & 0xFF) | v << 8);
    write(target, v);
}

void Nes::step() {
    if (tracer_) tracer_->instruction(pc_, static_cast<uint16_t>(0x100 | sp_));
    execute(fetch());
    // Interrupts are seen as the lines were at the end of the instruction's
    // second-to-last cycle.
    if (holdInterrupts_) {
        needNmi_ = prevNeedNmi_ = false;
        return;
    }
    if (prevRunIrq_ || prevNeedNmi_) interrupt();
}

void Nes::execute(uint8_t op) {
    const Mode m = kModes[op];
    const uint16_t addr = operand(m);
    switch (op) {
        case 0x00:
            brk();
            break;
        case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x15: case 0x19: case 0x1D:
            a_ |= value(m, addr);
            setZN(a_);
            break;
        case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2:
        case 0xD2: case 0xF2:
            // The processor locks up: it stays on this instruction.
            jammed_ = true;
            --pc_;
            break;
        case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:
            rmw(addr, [&](uint8_t v) { v = asl(v); a_ |= v; setZN(a_); return v; });
            break;
        case 0x04: case 0x0C: case 0x14: case 0x1A: case 0x1C: case 0x34: case 0x3A: case 0x3C: case 0x44: case 0x54:
        case 0x5A: case 0x5C: case 0x64: case 0x74: case 0x7A: case 0x7C: case 0x80: case 0x82: case 0x89: case 0xC2:
        case 0xD4: case 0xDA: case 0xDC: case 0xE2: case 0xEA: case 0xF4: case 0xFA: case 0xFC:
            if (m != Mode::Imp && m != Mode::Imm) read(addr);
            break;
        case 0x06: case 0x0E: case 0x16: case 0x1E:
            rmw(addr, [&](uint8_t v) { return asl(v); });
            break;
        case 0x08:
            push(static_cast<uint8_t>(p_ | B | U));
            break;
        case 0x0A:
            a_ = asl(a_);
            break;
        case 0x0B: case 0x2B:
            a_ &= value(m, addr);
            setZN(a_);
            flag(C, a_ & 0x80);
            break;
        case 0x10:
            branch(addr, !(p_ & N));
            break;
        case 0x18:
            p_ &= ~C;
            break;
        case 0x20: {
            const uint8_t lo = fetch();
            dummyRead(static_cast<uint16_t>(0x100 | sp_));
            push(static_cast<uint8_t>(pc_ >> 8));
            push(static_cast<uint8_t>(pc_));
            pc_ = static_cast<uint16_t>(lo | read(pc_) << 8);
            if (tracer_) tracer_->call(pc_, static_cast<uint16_t>(0x100 | sp_), false);
            break;
        }
        case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x35: case 0x39: case 0x3D:
            a_ &= value(m, addr);
            setZN(a_);
            break;
        case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F:
            rmw(addr, [&](uint8_t v) { v = rol(v); a_ &= v; setZN(a_); return v; });
            break;
        case 0x24: case 0x2C: {
            const uint8_t v = read(addr);
            p_ = static_cast<uint8_t>((p_ & 0x3D) | (v & 0xC0) | ((a_ & v) ? 0 : Z));
            break;
        }
        case 0x26: case 0x2E: case 0x36: case 0x3E:
            rmw(addr, [&](uint8_t v) { return rol(v); });
            break;
        case 0x28:
            dummyRead(static_cast<uint16_t>(0x100 | sp_));
            p_ = pop() & 0xCF;
            break;
        case 0x2A:
            a_ = rol(a_);
            break;
        case 0x30:
            branch(addr, p_ & N);
            break;
        case 0x38:
            p_ |= C;
            break;
        case 0x40: {
            dummyRead(static_cast<uint16_t>(0x100 | sp_));
            p_ = pop() & 0xCF;
            const uint8_t lo = pop();
            pc_ = static_cast<uint16_t>(lo | pop() << 8);
            break;
        }
        case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x55: case 0x59: case 0x5D:
            a_ ^= value(m, addr);
            setZN(a_);
            break;
        case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:
            rmw(addr, [&](uint8_t v) { v = lsr(v); a_ ^= v; setZN(a_); return v; });
            break;
        case 0x46: case 0x4E: case 0x56: case 0x5E:
            rmw(addr, [&](uint8_t v) { return lsr(v); });
            break;
        case 0x48:
            push(a_);
            break;
        case 0x4A:
            a_ = lsr(a_);
            break;
        case 0x4B:
            a_ &= value(m, addr);
            a_ = lsr(a_);
            break;
        case 0x4C:
            pc_ = addr;
            break;
        case 0x50:
            branch(addr, !(p_ & V));
            break;
        case 0x58:
            p_ &= ~I;
            break;
        case 0x60: {
            dummyRead(static_cast<uint16_t>(0x100 | sp_));
            const uint8_t lo = pop();
            pc_ = static_cast<uint16_t>(lo | pop() << 8);
            dummyRead(pc_++);
            break;
        }
        case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x75: case 0x79: case 0x7D:
            adc(value(m, addr));
            break;
        case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F:
            rmw(addr, [&](uint8_t v) { v = ror(v); adc(v); return v; });
            break;
        case 0x66: case 0x6E: case 0x76: case 0x7E:
            rmw(addr, [&](uint8_t v) { return ror(v); });
            break;
        case 0x68:
            dummyRead(static_cast<uint16_t>(0x100 | sp_));
            a_ = pop();
            setZN(a_);
            break;
        case 0x6A:
            a_ = ror(a_);
            break;
        case 0x6B:
            a_ = static_cast<uint8_t>(((a_ & value(m, addr)) >> 1) | (p_ & C ? 0x80 : 0));
            setZN(a_);
            flag(C, a_ & 0x40);
            flag(V, ((a_ >> 6) ^ (a_ >> 5)) & 1);
            break;
        case 0x6C: {
            const uint8_t lo = read(addr);
            pc_ = static_cast<uint16_t>(lo | read(static_cast<uint16_t>((addr & 0xFF00) | ((addr + 1) & 0xFF))) << 8);
            break;
        }
        case 0x70:
            branch(addr, p_ & V);
            break;
        case 0x78:
            p_ |= I;
            break;
        case 0x81: case 0x85: case 0x8D: case 0x91: case 0x95: case 0x99: case 0x9D:
            write(addr, a_);
            break;
        case 0x83: case 0x87: case 0x8F: case 0x97:
            write(addr, static_cast<uint8_t>(a_ & x_));
            break;
        case 0x84: case 0x8C: case 0x94:
            write(addr, y_);
            break;
        case 0x86: case 0x8E: case 0x96:
            write(addr, x_);
            break;
        case 0x88:
            setZN(--y_);
            break;
        case 0x8A:
            a_ = x_;
            setZN(a_);
            break;
        case 0x8B:
            a_ = static_cast<uint8_t>((a_ | 0xEE) & x_ & value(m, addr));
            setZN(a_);
            break;
        case 0x90:
            branch(addr, !(p_ & C));
            break;
        case 0x93: case 0x9F:
            storeHigh(y_, static_cast<uint8_t>(a_ & x_));
            break;
        case 0x98:
            a_ = y_;
            setZN(a_);
            break;
        case 0x9A:
            sp_ = x_;
            break;
        case 0x9B:
            sp_ = static_cast<uint8_t>(a_ & x_);
            storeHigh(y_, sp_);
            break;
        case 0x9C:
            storeHigh(x_, y_);
            break;
        case 0x9E:
            storeHigh(y_, x_);
            break;
        case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC:
            y_ = value(m, addr);
            setZN(y_);
            break;
        case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB5: case 0xB9: case 0xBD:
            a_ = value(m, addr);
            setZN(a_);
            break;
        case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
            x_ = value(m, addr);
            setZN(x_);
            break;
        case 0xA3: case 0xA7: case 0xAF: case 0xB3: case 0xB7: case 0xBF:
            a_ = x_ = value(m, addr);
            setZN(a_);
            break;
        case 0xA8:
            y_ = a_;
            setZN(y_);
            break;
        case 0xAA:
            x_ = a_;
            setZN(x_);
            break;
        case 0xAB:
            a_ = x_ = value(m, addr);
            setZN(a_);
            break;
        case 0xB0:
            branch(addr, p_ & C);
            break;
        case 0xB8:
            p_ &= ~V;
            break;
        case 0xBA:
            x_ = sp_;
            setZN(x_);
            break;
        case 0xBB:
            a_ = x_ = sp_ = static_cast<uint8_t>(value(m, addr) & sp_);
            setZN(a_);
            break;
        case 0xC0: case 0xC4: case 0xCC:
            compare(y_, value(m, addr));
            break;
        case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD5: case 0xD9: case 0xDD:
            compare(a_, value(m, addr));
            break;
        case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:
            rmw(addr, [&](uint8_t v) { v = static_cast<uint8_t>(v - 1); compare(a_, v); return v; });
            break;
        case 0xC6: case 0xCE: case 0xD6: case 0xDE:
            rmw(addr, [&](uint8_t v) { v = static_cast<uint8_t>(v - 1); setZN(v); return v; });
            break;
        case 0xC8:
            setZN(++y_);
            break;
        case 0xCA:
            setZN(--x_);
            break;
        case 0xCB: {
            const uint8_t v = value(m, addr), ax = static_cast<uint8_t>(a_ & x_);
            flag(C, ax >= v);
            x_ = static_cast<uint8_t>(ax - v);
            setZN(x_);
            break;
        }
        case 0xD0:
            branch(addr, !(p_ & Z));
            break;
        case 0xD8:
            p_ &= ~D;
            break;
        case 0xE0: case 0xE4: case 0xEC:
            compare(x_, value(m, addr));
            break;
        case 0xE1: case 0xE5: case 0xE9: case 0xEB: case 0xED: case 0xF1: case 0xF5: case 0xF9: case 0xFD:
            adc(static_cast<uint8_t>(value(m, addr) ^ 0xFF));
            break;
        case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:
            rmw(addr, [&](uint8_t v) { v = static_cast<uint8_t>(v + 1); adc(static_cast<uint8_t>(v ^ 0xFF)); return v; });
            break;
        case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            rmw(addr, [&](uint8_t v) { v = static_cast<uint8_t>(v + 1); setZN(v); return v; });
            break;
        case 0xE8:
            setZN(++x_);
            break;
        case 0xF0:
            branch(addr, p_ & Z);
            break;
        case 0xF8:
            p_ |= D;
            break;
    }
}

}  // namespace plumnin
