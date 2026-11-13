// The SPC700. The cycles of each instruction are the real one's, with its dummy reads
// (a store reads its address first, an implied instruction reads the next byte).
#include "snes_smp.h"

namespace plumnin::snes {

void Smp::power() {
    r = Regs{};
    halted_ = false;
}

uint8_t Smp::adc(uint8_t x, uint8_t y) {
    const int z = x + y + (r.p & C ? 1 : 0);
    flag(C, z > 0xFF);
    flag(H, (x ^ y ^ z) & 0x10);
    flag(V, ~(x ^ y) & (x ^ z) & 0x80);
    return zn(static_cast<uint8_t>(z));
}

uint8_t Smp::alu(Op op, uint8_t x, uint8_t y) {
    switch (op) {
        case Op::Or: return zn(static_cast<uint8_t>(x | y));
        case Op::And: return zn(static_cast<uint8_t>(x & y));
        case Op::Eor: return zn(static_cast<uint8_t>(x ^ y));
        case Op::Cmp: {
            const int z = x - y;
            flag(C, z >= 0);
            zn(static_cast<uint8_t>(z));
            return x;
        }
        case Op::Adc: return adc(x, y);
        case Op::Sbc: return adc(x, static_cast<uint8_t>(~y));
        case Op::Mov: return zn(y);
        case Op::Asl: flag(C, x & 0x80); return zn(static_cast<uint8_t>(x << 1));
        case Op::Rol: {
            const bool c = r.p & C;
            flag(C, x & 0x80);
            return zn(static_cast<uint8_t>(x << 1 | c));
        }
        case Op::Lsr: flag(C, x & 1); return zn(static_cast<uint8_t>(x >> 1));
        case Op::Ror: {
            const bool c = r.p & C;
            flag(C, x & 1);
            return zn(static_cast<uint8_t>(x >> 1 | (c ? 0x80 : 0)));
        }
        case Op::Dec: return zn(static_cast<uint8_t>(x - 1));
        case Op::Inc: return zn(static_cast<uint8_t>(x + 1));
    }
    return x;
}

void Smp::branch(bool take) {
    const int8_t offset = static_cast<int8_t>(fetch());
    if (!take) return;
    idle();
    idle();
    r.pc = static_cast<uint16_t>(r.pc + offset);
}

void Smp::step() {
    if (halted_) {
        idle();
        return;
    }
    execute(fetch());
}

void Smp::execute(uint8_t op) {
    const unsigned col = op & 0x0F, row = op >> 4;
    // OR AND EOR CMP ADC SBC, in rows 0 to B and columns 4 to 9.
    if (row < 0xC && col >= 4 && col <= 9) {
        static const Op kOps[6] = {Op::Or, Op::And, Op::Eor, Op::Cmp, Op::Adc, Op::Sbc};
        const Op o = kOps[row >> 1];
        const bool odd = row & 1;
        auto toA = [&](uint8_t v) { r.a = alu(o, r.a, v); };
        switch (col) {
            case 4: {
                const uint8_t d = fetch();
                if (odd) idle();
                toA(load(static_cast<uint8_t>(d + (odd ? r.x : 0))));
                return;
            }
            case 5: {
                const uint16_t a = fetch16();
                if (odd) idle();
                toA(read(static_cast<uint16_t>(a + (odd ? r.x : 0))));
                return;
            }
            case 6:
                if (!odd) {
                    read(r.pc);
                    toA(load(r.x));
                } else {
                    const uint16_t a = fetch16();
                    idle();
                    toA(read(static_cast<uint16_t>(a + r.y)));
                }
                return;
            case 7: {
                const uint8_t d = fetch();
                uint16_t a;
                if (!odd) {
                    idle();
                    const uint8_t lo = load(static_cast<uint8_t>(d + r.x));
                    a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + r.x + 1)) << 8);
                } else {
                    // The read of [d]+Y waits before the pointer, the write after it.
                    idle();
                    const uint8_t lo = load(d);
                    a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + 1)) << 8);
                    a = static_cast<uint16_t>(a + r.y);
                }
                toA(read(a));
                return;
            }
            case 8:
                if (!odd) {
                    toA(fetch());
                } else {
                    const uint8_t imm = fetch();
                    const uint8_t d = fetch();
                    const uint8_t v = alu(o, load(d), imm);
                    if (o == Op::Cmp) idle();
                    else store(d, v);
                }
                return;
            default:
                if (!odd) {
                    const uint8_t s = fetch();
                    const uint8_t rhs = load(s);
                    const uint8_t t = fetch();
                    const uint8_t v = alu(o, load(t), rhs);
                    if (o == Op::Cmp) idle();
                    else store(t, v);
                } else {
                    read(r.pc);
                    const uint8_t rhs = load(r.y);
                    const uint8_t v = alu(o, load(r.x), rhs);
                    if (o == Op::Cmp) idle();
                    else store(r.x, v);
                }
                return;
        }
    }
    // ASL ROL LSR ROR DEC INC on the direct page, an address or a register.
    if (row < 0xC && (col == 0xB || col == 0xC)) {
        static const Op kOps[6] = {Op::Asl, Op::Rol, Op::Lsr, Op::Ror, Op::Dec, Op::Inc};
        const Op o = kOps[row >> 1];
        const bool odd = row & 1;
        if (col == 0xB) {
            const uint8_t d = fetch();
            if (odd) idle();
            const uint8_t at = static_cast<uint8_t>(d + (odd ? r.x : 0));
            store(at, alu(o, load(at), 0));
        } else if (!odd) {
            const uint16_t a = fetch16();
            write(a, alu(o, read(a), 0));
        } else {
            read(r.pc);
            r.a = alu(o, r.a, 0);
        }
        return;
    }
    if (col == 1) {  // TCALL n
        read(r.pc);
        idle();
        push(static_cast<uint8_t>(r.pc >> 8));
        push(static_cast<uint8_t>(r.pc));
        idle();
        const uint16_t at = static_cast<uint16_t>(0xFFDE - (row << 1));
        const uint8_t lo = read(at);
        r.pc = static_cast<uint16_t>(lo | read(static_cast<uint16_t>(at + 1)) << 8);
        return;
    }
    if (col == 2) {  // SET1 / CLR1 d.n
        const uint8_t d = fetch();
        uint8_t v = load(d);
        const uint8_t bit = static_cast<uint8_t>(1 << (row >> 1));
        v = row & 1 ? static_cast<uint8_t>(v & ~bit) : static_cast<uint8_t>(v | bit);
        store(d, v);
        return;
    }
    if (col == 3) {  // BBS / BBC d.n, r
        const uint8_t d = fetch();
        const uint8_t v = load(d);
        idle();
        const int8_t offset = static_cast<int8_t>(fetch());
        const bool set = v & (1 << (row >> 1));
        if (set == !(row & 1)) {
            idle();
            idle();
            r.pc = static_cast<uint16_t>(r.pc + offset);
        }
        return;
    }

    auto implied = [&] { read(r.pc); };
    auto transfer = [&](uint8_t& to, uint8_t from) {
        read(r.pc);
        to = zn(from);
    };
    auto directWrite = [&](uint8_t v, uint8_t index, bool indexed) {
        const uint8_t d = fetch();
        if (indexed) idle();
        const uint8_t at = static_cast<uint8_t>(d + index);
        load(at);
        store(at, v);
    };
    auto directRead = [&](uint8_t& to, Op o, uint8_t index, bool indexed) {
        const uint8_t d = fetch();
        if (indexed) idle();
        to = alu(o, to, load(static_cast<uint8_t>(d + index)));
    };
    auto absoluteWrite = [&](uint8_t v, uint8_t index, bool indexed) {
        const uint16_t a = fetch16();
        if (indexed) idle();
        const uint16_t at = static_cast<uint16_t>(a + index);
        read(at);
        write(at, v);
    };
    auto absoluteRead = [&](uint8_t& to, Op o, uint8_t index, bool indexed) {
        const uint16_t a = fetch16();
        if (indexed) idle();
        to = alu(o, to, read(static_cast<uint16_t>(a + index)));
    };
    auto bitAddress = [&](uint16_t& a, uint8_t& bit) {
        const uint16_t v = fetch16();
        bit = static_cast<uint8_t>(v >> 13);
        a = v & 0x1FFF;
    };

    switch (op) {
        case 0x00: implied(); break;
        case 0x10: branch(!(r.p & N)); break;
        case 0x30: branch(r.p & N); break;
        case 0x50: branch(!(r.p & V)); break;
        case 0x70: branch(r.p & V); break;
        case 0x90: branch(!(r.p & C)); break;
        case 0xB0: branch(r.p & C); break;
        case 0xD0: branch(!(r.p & Z)); break;
        case 0xF0: branch(r.p & Z); break;
        case 0x2F: branch(true); break;
        case 0x20: implied(), flag(P, false); break;
        case 0x40: implied(), flag(P, true); break;
        case 0x60: implied(), flag(C, false); break;
        case 0x80: implied(), flag(C, true); break;
        case 0xE0: implied(), flag(V, false), flag(H, false); break;
        case 0xA0: implied(), idle(), flag(I, true); break;
        case 0xC0: implied(), idle(), flag(I, false); break;
        case 0xED: implied(), idle(), flag(C, !(r.p & C)); break;

        // The C8-FF block of moves and compares with X and Y.
        case 0xC4: directWrite(r.a, 0, false); break;
        case 0xD4: directWrite(r.a, r.x, true); break;
        case 0xD8: directWrite(r.x, 0, false); break;
        case 0xD9: directWrite(r.x, r.y, true); break;
        case 0xCB: directWrite(r.y, 0, false); break;
        case 0xDB: directWrite(r.y, r.x, true); break;
        case 0xE4: directRead(r.a, Op::Mov, 0, false); break;
        case 0xF4: directRead(r.a, Op::Mov, r.x, true); break;
        case 0xF8: directRead(r.x, Op::Mov, 0, false); break;
        case 0xF9: directRead(r.x, Op::Mov, r.y, true); break;
        case 0xEB: directRead(r.y, Op::Mov, 0, false); break;
        case 0xFB: directRead(r.y, Op::Mov, r.x, true); break;
        case 0x3E: directRead(r.x, Op::Cmp, 0, false); break;
        case 0x7E: directRead(r.y, Op::Cmp, 0, false); break;
        case 0xC5: absoluteWrite(r.a, 0, false); break;
        case 0xD5: absoluteWrite(r.a, r.x, true); break;
        case 0xD6: absoluteWrite(r.a, r.y, true); break;
        case 0xC9: absoluteWrite(r.x, 0, false); break;
        case 0xCC: absoluteWrite(r.y, 0, false); break;
        case 0xE5: absoluteRead(r.a, Op::Mov, 0, false); break;
        case 0xF5: absoluteRead(r.a, Op::Mov, r.x, true); break;
        case 0xF6: absoluteRead(r.a, Op::Mov, r.y, true); break;
        case 0xE9: absoluteRead(r.x, Op::Mov, 0, false); break;
        case 0xEC: absoluteRead(r.y, Op::Mov, 0, false); break;
        case 0x1E: absoluteRead(r.x, Op::Cmp, 0, false); break;
        case 0x5E: absoluteRead(r.y, Op::Cmp, 0, false); break;
        case 0xE8: r.a = zn(fetch()); break;
        case 0xCD: r.x = zn(fetch()); break;
        case 0x8D: r.y = zn(fetch()); break;
        case 0xC8: alu(Op::Cmp, r.x, fetch()); break;
        case 0xAD: alu(Op::Cmp, r.y, fetch()); break;
        case 0xC6:
            implied();
            load(r.x);
            store(r.x, r.a);
            break;
        case 0xE6:
            implied();
            r.a = zn(load(r.x));
            break;
        case 0xAF:
            implied();
            idle();
            store(r.x++, r.a);
            break;
        case 0xBF:
            implied();
            r.a = zn(load(r.x++));
            idle();
            break;
        case 0xC7:
        case 0xD7: {
            const uint8_t d = fetch();
            uint16_t a;
            if (op == 0xC7) {
                idle();
                const uint8_t lo = load(static_cast<uint8_t>(d + r.x));
                a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + r.x + 1)) << 8);
            } else {
                const uint8_t lo = load(d);
                a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + 1)) << 8);
                idle();
                a = static_cast<uint16_t>(a + r.y);
            }
            read(a);
            write(a, r.a);
            break;
        }
        case 0xE7:
        case 0xF7: {
            const uint8_t d = fetch();
            uint16_t a;
            if (op == 0xE7) {
                idle();
                const uint8_t lo = load(static_cast<uint8_t>(d + r.x));
                a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + r.x + 1)) << 8);
            } else {
                idle();
                const uint8_t lo = load(d);
                a = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + 1)) << 8);
                a = static_cast<uint16_t>(a + r.y);
            }
            r.a = zn(read(a));
            break;
        }
        case 0x8F: {
            const uint8_t imm = fetch();
            const uint8_t d = fetch();
            load(d);
            store(d, imm);
            break;
        }
        case 0xFA: {
            const uint8_t s = fetch();
            const uint8_t v = load(s);
            const uint8_t d = fetch();
            store(d, v);
            break;
        }

        case 0x5D: transfer(r.x, r.a); break;
        case 0x7D: transfer(r.a, r.x); break;
        case 0x9D: transfer(r.x, r.sp); break;
        case 0xBD: implied(), r.sp = r.x; break;
        case 0xDD: transfer(r.a, r.y); break;
        case 0xFD: transfer(r.y, r.a); break;
        case 0x1D: transfer(r.x, static_cast<uint8_t>(r.x - 1)); break;
        case 0x3D: transfer(r.x, static_cast<uint8_t>(r.x + 1)); break;
        case 0xDC: transfer(r.y, static_cast<uint8_t>(r.y - 1)); break;
        case 0xFC: transfer(r.y, static_cast<uint8_t>(r.y + 1)); break;

        case 0x0D: implied(), push(r.p), idle(); break;
        case 0x2D: implied(), push(r.a), idle(); break;
        case 0x4D: implied(), push(r.x), idle(); break;
        case 0x6D: implied(), push(r.y), idle(); break;
        case 0x8E: implied(), idle(), r.p = pull(); break;
        case 0xAE: implied(), idle(), r.a = pull(); break;
        case 0xCE: implied(), idle(), r.x = pull(); break;
        case 0xEE: implied(), idle(), r.y = pull(); break;

        case 0x0E:
        case 0x4E: {  // TSET1 / TCLR1 !a
            const uint16_t a = fetch16();
            const uint8_t v = read(a);
            zn(static_cast<uint8_t>(r.a - v));
            read(a);
            write(a, op == 0x0E ? static_cast<uint8_t>(v | r.a) : static_cast<uint8_t>(v & ~r.a));
            break;
        }
        case 0x2E:
        case 0xDE: {  // CBNE d / d+X, r
            const uint8_t d = fetch();
            if (op == 0xDE) idle();
            const uint8_t v = load(static_cast<uint8_t>(d + (op == 0xDE ? r.x : 0)));
            idle();
            const int8_t offset = static_cast<int8_t>(fetch());
            if (r.a != v) {
                idle();
                idle();
                r.pc = static_cast<uint16_t>(r.pc + offset);
            }
            break;
        }
        case 0x6E: {  // DBNZ d, r
            const uint8_t d = fetch();
            const uint8_t v = static_cast<uint8_t>(load(d) - 1);
            store(d, v);
            const int8_t offset = static_cast<int8_t>(fetch());
            if (v) {
                idle();
                idle();
                r.pc = static_cast<uint16_t>(r.pc + offset);
            }
            break;
        }
        case 0xFE: {  // DBNZ Y, r
            implied();
            idle();
            const int8_t offset = static_cast<int8_t>(fetch());
            if (--r.y) {
                idle();
                idle();
                r.pc = static_cast<uint16_t>(r.pc + offset);
            }
            break;
        }

        case 0x1A:
        case 0x3A: {  // DECW / INCW d
            const uint8_t d = fetch();
            uint16_t v = static_cast<uint16_t>(load(d) + (op == 0x3A ? 1 : -1));
            store(d, static_cast<uint8_t>(v));
            v = static_cast<uint16_t>(v + (load(static_cast<uint8_t>(d + 1)) << 8));
            store(static_cast<uint8_t>(d + 1), static_cast<uint8_t>(v >> 8));
            flag(Z, !v);
            flag(N, v & 0x8000);
            break;
        }
        case 0x5A: {  // CMPW YA, d
            const uint8_t d = fetch();
            const uint8_t lo = load(d);
            const uint16_t v = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + 1)) << 8);
            const int z = ya() - v;
            flag(C, z >= 0);
            flag(Z, !static_cast<uint16_t>(z));
            flag(N, z & 0x8000);
            break;
        }
        case 0x7A:
        case 0x9A:
        case 0xBA: {  // ADDW / SUBW / MOVW YA, d
            const uint8_t d = fetch();
            const uint8_t lo = load(d);
            idle();
            const uint16_t v = static_cast<uint16_t>(lo | load(static_cast<uint8_t>(d + 1)) << 8);
            if (op == 0xBA) {
                setYa(v);
                flag(Z, !v);
                flag(N, v & 0x8000);
            } else {
                const uint16_t x = ya();
                flag(C, op == 0x9A);
                const uint8_t y = op == 0x9A ? static_cast<uint8_t>(~v) : static_cast<uint8_t>(v);
                const uint8_t yh = op == 0x9A ? static_cast<uint8_t>(~v >> 8) : static_cast<uint8_t>(v >> 8);
                const uint8_t zl = adc(static_cast<uint8_t>(x), y);
                const uint8_t zh = adc(static_cast<uint8_t>(x >> 8), yh);
                setYa(static_cast<uint16_t>(zh << 8 | zl));
                flag(Z, !ya());
            }
            break;
        }
        case 0xDA: {  // MOVW d, YA
            const uint8_t d = fetch();
            load(d);
            store(d, r.a);
            store(static_cast<uint8_t>(d + 1), r.y);
            break;
        }

        case 0x0A:
        case 0x2A:
        case 0x4A:
        case 0x6A:
        case 0x8A:
        case 0xAA:
        case 0xCA:
        case 0xEA: {  // the bit instructions on m.b
            uint16_t a;
            uint8_t bit;
            bitAddress(a, bit);
            uint8_t v = read(a);
            const bool b = v & (1 << bit);
            const bool c = r.p & C;
            switch (op) {
                case 0x0A: idle(), flag(C, c || b); break;
                case 0x2A: idle(), flag(C, c || !b); break;
                case 0x4A: flag(C, c && b); break;
                case 0x6A: flag(C, c && !b); break;
                case 0x8A: idle(), flag(C, c != b); break;
                case 0xAA: flag(C, b); break;
                case 0xCA:
                    idle();
                    v = c ? static_cast<uint8_t>(v | 1 << bit) : static_cast<uint8_t>(v & ~(1 << bit));
                    write(a, v);
                    break;
                default: write(a, static_cast<uint8_t>(v ^ (1 << bit))); break;
            }
            break;
        }

        case 0x0F: {  // BRK
            implied();
            push(static_cast<uint8_t>(r.pc >> 8));
            push(static_cast<uint8_t>(r.pc));
            push(r.p);
            idle();
            const uint8_t lo = read(0xFFDE);
            r.pc = static_cast<uint16_t>(lo | read(0xFFDF) << 8);
            flag(I, false);
            flag(B, true);
            break;
        }
        case 0x1F: {  // JMP [!a+X]
            const uint16_t a = static_cast<uint16_t>(fetch16());
            idle();
            const uint16_t at = static_cast<uint16_t>(a + r.x);
            const uint8_t lo = read(at);
            r.pc = static_cast<uint16_t>(lo | read(static_cast<uint16_t>(at + 1)) << 8);
            break;
        }
        case 0x3F: {  // CALL !a
            const uint16_t a = fetch16();
            idle();
            push(static_cast<uint8_t>(r.pc >> 8));
            push(static_cast<uint8_t>(r.pc));
            idle();
            idle();
            r.pc = a;
            break;
        }
        case 0x4F: {  // PCALL u
            const uint8_t u = fetch();
            idle();
            push(static_cast<uint8_t>(r.pc >> 8));
            push(static_cast<uint8_t>(r.pc));
            idle();
            r.pc = static_cast<uint16_t>(0xFF00 | u);
            break;
        }
        case 0x5F: r.pc = fetch16(); break;
        case 0x6F: {
            implied();
            idle();
            const uint8_t lo = pull();
            r.pc = static_cast<uint16_t>(lo | pull() << 8);
            break;
        }
        case 0x7F: {
            implied();
            idle();
            r.p = pull();
            const uint8_t lo = pull();
            r.pc = static_cast<uint16_t>(lo | pull() << 8);
            break;
        }
        case 0x9E: {  // DIV YA, X
            implied();
            for (int i = 0; i < 10; ++i) idle();
            const unsigned v = ya(), x = r.x, y = r.y;
            flag(H, (y & 15) >= (x & 15));
            flag(V, y >= x);
            if (y < (x << 1)) {
                r.a = static_cast<uint8_t>(v / x);
                r.y = static_cast<uint8_t>(v % x);
            } else {
                r.a = static_cast<uint8_t>(255 - (v - (x << 9)) / (256 - x));
                r.y = static_cast<uint8_t>(x + (v - (x << 9)) % (256 - x));
            }
            zn(r.a);
            break;
        }
        case 0xCF: {  // MUL YA
            implied();
            for (int i = 0; i < 7; ++i) idle();
            setYa(static_cast<uint16_t>(r.y * r.a));
            zn(r.y);
            break;
        }
        case 0x9F:
            implied();
            idle();
            idle();
            idle();
            r.a = zn(static_cast<uint8_t>(r.a >> 4 | r.a << 4));
            break;
        case 0xBE:  // DAS
            implied();
            idle();
            if (!(r.p & C) || r.a > 0x99) {
                r.a = static_cast<uint8_t>(r.a - 0x60);
                flag(C, false);
            }
            if (!(r.p & H) || (r.a & 15) > 9) r.a = static_cast<uint8_t>(r.a - 6);
            zn(r.a);
            break;
        case 0xDF:  // DAA
            implied();
            idle();
            if ((r.p & C) || r.a > 0x99) {
                r.a = static_cast<uint8_t>(r.a + 0x60);
                flag(C, true);
            }
            if ((r.p & H) || (r.a & 15) > 9) r.a = static_cast<uint8_t>(r.a + 6);
            zn(r.a);
            break;
        case 0xEF:
        case 0xFF:
            implied();
            idle();
            halted_ = true;
            break;
        default: implied(); break;
    }
}

}  // namespace plumnin::snes
