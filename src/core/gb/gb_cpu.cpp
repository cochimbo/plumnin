// The Game Boy's processor, the Sharp SM83: one M-cycle of the rest of the console
// runs with every memory access (read() and write() tick) and with every internal
// cycle (a bare tick()), so each instruction takes as long as on the console.

#include "gb.h"

namespace plumnin {

namespace {

constexpr uint8_t kZ = 0x80, kN = 0x40, kH = 0x20, kC = 0x10;

}  // namespace

void GameBoy::push(uint16_t v) {
    --sp_;
    write(sp_, static_cast<uint8_t>(v >> 8));
    --sp_;
    write(sp_, static_cast<uint8_t>(v));
}

uint16_t GameBoy::pop() {
    const uint8_t lo = read(sp_++);
    const uint8_t hi = read(sp_++);
    return static_cast<uint16_t>(hi << 8 | lo);
}

uint8_t& GameBoy::reg(int r) {
    switch (r) {
        case 0: return b_;
        case 1: return c_;
        case 2: return d_;
        case 3: return e_;
        case 4: return h_;
        case 5: return l_;
        default: return a_;
    }
}

uint8_t GameBoy::getR(int r) {
    if (r == 6) return read(static_cast<uint16_t>(h_ << 8 | l_));
    return reg(r);
}

void GameBoy::setR(int r, uint8_t v) {
    if (r == 6) write(static_cast<uint16_t>(h_ << 8 | l_), v);
    else reg(r) = v;
}

bool GameBoy::condition(int cc) const {
    switch (cc & 3) {
        case 0: return !(f_ & kZ);
        case 1: return f_ & kZ;
        case 2: return !(f_ & kC);
        default: return f_ & kC;
    }
}

void GameBoy::alu(int op, uint8_t v) {
    const int carry = (f_ & kC) ? 1 : 0;
    int r;
    switch (op) {
        case 0:  // ADD
        case 1: {  // ADC
            const int c = op == 1 ? carry : 0;
            r = a_ + v + c;
            f_ = static_cast<uint8_t>(((r & 0xFF) ? 0 : kZ) | (((a_ & 0xF) + (v & 0xF) + c) > 0xF ? kH : 0) |
                                      (r > 0xFF ? kC : 0));
            a_ = static_cast<uint8_t>(r);
            break;
        }
        case 2:  // SUB
        case 3:  // SBC
        case 7: {  // CP
            const int c = op == 3 ? carry : 0;
            r = a_ - v - c;
            f_ = static_cast<uint8_t>(((r & 0xFF) ? 0 : kZ) | kN | ((a_ & 0xF) < (v & 0xF) + c ? kH : 0) |
                                      (r < 0 ? kC : 0));
            if (op != 7) a_ = static_cast<uint8_t>(r);
            break;
        }
        case 4:
            a_ &= v;
            f_ = static_cast<uint8_t>((a_ ? 0 : kZ) | kH);
            break;
        case 5:
            a_ ^= v;
            f_ = a_ ? 0 : kZ;
            break;
        default:
            a_ |= v;
            f_ = a_ ? 0 : kZ;
            break;
    }
}

uint8_t GameBoy::inc(uint8_t v) {
    const uint8_t r = static_cast<uint8_t>(v + 1);
    f_ = static_cast<uint8_t>((f_ & kC) | (r ? 0 : kZ) | ((v & 0xF) == 0xF ? kH : 0));
    return r;
}

uint8_t GameBoy::dec(uint8_t v) {
    const uint8_t r = static_cast<uint8_t>(v - 1);
    f_ = static_cast<uint8_t>((f_ & kC) | kN | (r ? 0 : kZ) | ((v & 0xF) == 0 ? kH : 0));
    return r;
}

uint8_t GameBoy::cb(int op, uint8_t v) {
    const int carry = (f_ & kC) ? 1 : 0;
    uint8_t r;
    bool c;
    switch (op) {
        case 0: c = v & 0x80, r = static_cast<uint8_t>(v << 1 | v >> 7); break;           // RLC
        case 1: c = v & 1, r = static_cast<uint8_t>(v >> 1 | v << 7); break;              // RRC
        case 2: c = v & 0x80, r = static_cast<uint8_t>(v << 1 | carry); break;            // RL
        case 3: c = v & 1, r = static_cast<uint8_t>(v >> 1 | carry << 7); break;          // RR
        case 4: c = v & 0x80, r = static_cast<uint8_t>(v << 1); break;                    // SLA
        case 5: c = v & 1, r = static_cast<uint8_t>((v >> 1) | (v & 0x80)); break;        // SRA
        case 6: c = false, r = static_cast<uint8_t>(v << 4 | v >> 4); break;              // SWAP
        default: c = v & 1, r = static_cast<uint8_t>(v >> 1); break;                      // SRL
    }
    f_ = static_cast<uint8_t>((r ? 0 : kZ) | (c ? kC : 0));
    return r;
}

void GameBoy::interrupt() {
    ime_ = false;
    tick();
    tick();
    --sp_;
    write(sp_, static_cast<uint8_t>(pc_ >> 8));
    // Which interrupt is served is decided after the high byte went: a push that
    // overwrote IE can cancel it, and the processor then jumps to 0000.
    const uint8_t pending = ie_ & if_ & 0x1F;
    --sp_;
    write(sp_, static_cast<uint8_t>(pc_));
    if (!pending) {
        pc_ = 0;
    } else {
        int bit = 0;
        while (!(pending & (1 << bit))) ++bit;
        if_ &= static_cast<uint8_t>(~(1 << bit));
        pc_ = static_cast<uint16_t>(0x40 + bit * 8);
    }
    tick();
}

void GameBoy::step() {
    if (ppu_.takeHblank() && hdmaActive_ && !halted_) {
        hdmaBlock();
        if (hdmaLength_ == 0) {
            hdmaActive_ = false;
            hdmaLength_ = 0x7F;
        } else {
            --hdmaLength_;
        }
    }
    if (stopped_) {
        tick();
        return;
    }
    const uint8_t pending = ie_ & if_ & 0x1F;
    if (halted_) {
        if (!pending) {
            tick();
            return;
        }
        halted_ = false;
    }
    if (ime_ && pending) {
        interrupt();
        return;
    }
    if (eiDelay_) {
        eiDelay_ = false;
        ime_ = true;
    }
    execute(fetch());
}

void GameBoy::execute(uint8_t op) {
    auto hl = [&] { return static_cast<uint16_t>(h_ << 8 | l_); };
    auto setHl = [&](uint16_t v) { h_ = static_cast<uint8_t>(v >> 8), l_ = static_cast<uint8_t>(v); };
    auto rr = [&](int i) -> uint16_t {
        switch (i) {
            case 0: return static_cast<uint16_t>(b_ << 8 | c_);
            case 1: return static_cast<uint16_t>(d_ << 8 | e_);
            case 2: return hl();
            default: return sp_;
        }
    };
    auto setRr = [&](int i, uint16_t v) {
        switch (i) {
            case 0: b_ = static_cast<uint8_t>(v >> 8), c_ = static_cast<uint8_t>(v); break;
            case 1: d_ = static_cast<uint8_t>(v >> 8), e_ = static_cast<uint8_t>(v); break;
            case 2: setHl(v); break;
            default: sp_ = v; break;
        }
    };
    auto spPlus = [&]() {
        const int8_t e = static_cast<int8_t>(fetch());
        const uint8_t u = static_cast<uint8_t>(e);
        f_ = static_cast<uint8_t>((((sp_ & 0xF) + (u & 0xF)) > 0xF ? kH : 0) | (((sp_ & 0xFF) + u) > 0xFF ? kC : 0));
        return static_cast<uint16_t>(sp_ + e);
    };

    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) {  // HALT
            if (!ime_ && (ie_ & if_ & 0x1F)) haltBug_ = true;  // the next byte is read twice
            else halted_ = true;
            return;
        }
        if (op == 0x40) breakpoint_ = true;  // LD B,B: the test ROMs' "done"
        setR((op >> 3) & 7, getR(op & 7));
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu((op >> 3) & 7, getR(op & 7));
        return;
    }
    switch (op) {
        case 0x00: return;
        case 0x01: case 0x11: case 0x21: case 0x31: setRr(op >> 4, fetch16()); return;
        case 0x02: write(rr(0), a_); return;
        case 0x12: write(rr(1), a_); return;
        case 0x22: write(hl(), a_), setHl(static_cast<uint16_t>(hl() + 1)); return;
        case 0x32: write(hl(), a_), setHl(static_cast<uint16_t>(hl() - 1)); return;
        case 0x0A: a_ = read(rr(0)); return;
        case 0x1A: a_ = read(rr(1)); return;
        case 0x2A: a_ = read(hl()), setHl(static_cast<uint16_t>(hl() + 1)); return;
        case 0x3A: a_ = read(hl()), setHl(static_cast<uint16_t>(hl() - 1)); return;
        case 0x03: case 0x13: case 0x23: case 0x33:
            setRr(op >> 4, static_cast<uint16_t>(rr(op >> 4) + 1));
            tick();
            return;
        case 0x0B: case 0x1B: case 0x2B: case 0x3B:
            setRr(op >> 4, static_cast<uint16_t>(rr(op >> 4) - 1));
            tick();
            return;
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            const int r = (op >> 3) & 7;
            setR(r, inc(getR(r)));
            return;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            const int r = (op >> 3) & 7;
            setR(r, dec(getR(r)));
            return;
        }
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: {
            const uint8_t n = fetch();
            setR((op >> 3) & 7, n);
            return;
        }
        case 0x07: a_ = cb(0, a_), f_ &= kC; return;  // RLCA
        case 0x0F: a_ = cb(1, a_), f_ &= kC; return;  // RRCA
        case 0x17: a_ = cb(2, a_), f_ &= kC; return;  // RLA
        case 0x1F: a_ = cb(3, a_), f_ &= kC; return;  // RRA
        case 0x08: {
            const uint16_t at = fetch16();
            write(at, static_cast<uint8_t>(sp_));
            write(static_cast<uint16_t>(at + 1), static_cast<uint8_t>(sp_ >> 8));
            return;
        }
        case 0x09: case 0x19: case 0x29: case 0x39: {
            const uint16_t x = hl(), y = rr(op >> 4);
            const int r = x + y;
            f_ = static_cast<uint8_t>((f_ & kZ) | (((x & 0xFFF) + (y & 0xFFF)) > 0xFFF ? kH : 0) | (r > 0xFFFF ? kC : 0));
            setHl(static_cast<uint16_t>(r));
            tick();
            return;
        }
        case 0x10: {  // STOP: the Color's speed switch, or sleep until a button
            fetch();
            if (color_ && (key1_ & 1)) {
                doubleSpeed_ = !doubleSpeed_;
                key1_ = 0;
                div_ = 0;
            } else {
                stopped_ = true;
                div_ = 0;
            }
            return;
        }
        case 0x18: {
            const int8_t e = static_cast<int8_t>(fetch());
            pc_ = static_cast<uint16_t>(pc_ + e);
            tick();
            return;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {
            const int8_t e = static_cast<int8_t>(fetch());
            if (condition((op >> 3) & 3)) {
                pc_ = static_cast<uint16_t>(pc_ + e);
                tick();
            }
            return;
        }
        case 0x27: {  // DAA
            int a = a_;
            bool carry = f_ & kC;
            if (!(f_ & kN)) {
                if (carry || a > 0x99) a += 0x60, carry = true;
                if ((f_ & kH) || (a & 0xF) > 9) a += 6;
            } else {
                if (carry) a -= 0x60;
                if (f_ & kH) a -= 6;
            }
            a_ = static_cast<uint8_t>(a);
            f_ = static_cast<uint8_t>((f_ & kN) | (a_ ? 0 : kZ) | (carry ? kC : 0));
            return;
        }
        case 0x2F: a_ = static_cast<uint8_t>(~a_), f_ |= kN | kH; return;
        case 0x37: f_ = static_cast<uint8_t>((f_ & kZ) | kC); return;
        case 0x3F: f_ = static_cast<uint8_t>((f_ & (kZ | kC)) ^ kC); return;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            tick();
            if (condition((op >> 3) & 3)) {
                pc_ = pop();
                tick();
            }
            return;
        case 0xC1: case 0xD1: case 0xE1: setRr((op >> 4) & 3, pop()); return;
        case 0xF1: {
            const uint16_t v = pop();
            a_ = static_cast<uint8_t>(v >> 8);
            f_ = static_cast<uint8_t>(v & 0xF0);
            return;
        }
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
            const uint16_t to = fetch16();
            if (condition((op >> 3) & 3)) {
                pc_ = to;
                tick();
            }
            return;
        }
        case 0xC3: pc_ = fetch16(), tick(); return;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
            const uint16_t to = fetch16();
            if (condition((op >> 3) & 3)) {
                tick();
                push(pc_);
                pc_ = to;
            }
            return;
        }
        case 0xC5: case 0xD5: case 0xE5: tick(), push(rr((op >> 4) & 3)); return;
        case 0xF5: tick(), push(static_cast<uint16_t>(a_ << 8 | f_)); return;
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            alu((op >> 3) & 7, fetch());
            return;
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            tick();
            push(pc_);
            pc_ = op & 0x38;
            return;
        case 0xC9: pc_ = pop(), tick(); return;
        case 0xD9: pc_ = pop(), tick(), ime_ = true; return;
        case 0xCB: executeCb(); return;
        case 0xCD: {
            const uint16_t to = fetch16();
            tick();
            push(pc_);
            pc_ = to;
            return;
        }
        case 0xE0: {
            const uint8_t n = fetch();
            write(static_cast<uint16_t>(0xFF00 | n), a_);
            return;
        }
        case 0xF0: {
            const uint8_t n = fetch();
            a_ = read(static_cast<uint16_t>(0xFF00 | n));
            return;
        }
        case 0xE2: write(static_cast<uint16_t>(0xFF00 | c_), a_); return;
        case 0xF2: a_ = read(static_cast<uint16_t>(0xFF00 | c_)); return;
        case 0xE8: sp_ = spPlus(), tick(), tick(); return;
        case 0xF8: setHl(spPlus()), tick(); return;
        case 0xE9: pc_ = hl(); return;
        case 0xF9: sp_ = hl(), tick(); return;
        case 0xEA: write(fetch16(), a_); return;
        case 0xFA: a_ = read(fetch16()); return;
        case 0xF3: ime_ = false, eiDelay_ = false; return;
        case 0xFB: eiDelay_ = true; return;
        default:
            // The opcodes the SM83 lacks lock it up.
            --pc_;
            ime_ = false;
            halted_ = true;
            ie_ = 0;
            return;
    }
}

void GameBoy::executeCb() {
    const uint8_t op = fetch();
    const int r = op & 7, n = (op >> 3) & 7;
    const uint8_t v = getR(r);
    switch (op >> 6) {
        case 0: setR(r, cb(n, v)); break;
        case 1: f_ = static_cast<uint8_t>((f_ & kC) | kH | ((v >> n) & 1 ? 0 : kZ)); break;
        case 2: setR(r, static_cast<uint8_t>(v & ~(1 << n))); break;
        default: setR(r, static_cast<uint8_t>(v | 1 << n)); break;
    }
}

}  // namespace plumnin
