#include "snes_necdsp.h"

namespace plumnin::snes {

namespace {

// How much the words read one way look like a uPD77C25 program: jumps use only the
// conditions there are, and loads leave bits 4 and 5 clear. Firmware dumps come with
// each instruction's bytes in either order; the right one scores far higher.
int programScore(const std::vector<uint32_t>& program) {
    int score = 0;
    for (uint32_t op : program) {
        if (op == 0xFFFFFF || op == 0) continue;
        switch (op >> 22) {
            case 2: {
                const uint32_t brch = (op >> 13) & 0x1FF;
                const bool known = brch == 0 || (brch >= 0x080 && brch <= 0x0BE && (brch < 0x0B0 || brch > 0x0B3) && !(brch & 1)) ||
                                   (brch >= 0x0B0 && brch <= 0x0B3) || brch == 0x100 || brch == 0x101 || brch == 0x140 ||
                                   brch == 0x141;
                score += known ? 1 : -1;
                break;
            }
            case 3: score += (op & 0x30) ? -1 : 1; break;
            default: break;
        }
    }
    return score;
}

}  // namespace

void NecDsp::setup(Model model, Map map, double hz) {
    model_ = model;
    map_ = map;
    hz_ = static_cast<int64_t>(hz);
    const bool big = model == Model::Upd96050;
    pcMask_ = big ? 0x3FFF : 0x7FF;
    rpMask_ = big ? 0x7FF : 0x3FF;
    dpMask_ = big ? 0x7FF : 0xFF;
    stackMask_ = big ? 15 : 3;
}

bool NecDsp::setFirmware(Model model, Map map, const std::vector<uint8_t>& firmware, double hz) {
    if (firmware.size() != firmwareSize(model)) return false;
    const size_t words = model == Model::Upd7725 ? 2048 : 16384, data = model == Model::Upd7725 ? 1024 : 2048;
    std::vector<uint32_t> big(words), little(words);
    for (size_t i = 0; i < words; ++i) {
        const uint8_t* p = &firmware[i * 3];
        big[i] = static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
        little[i] = static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[0];
    }
    const bool msbFirst = programScore(big) > programScore(little);
    std::vector<uint16_t> rom(data);
    for (size_t i = 0; i < data; ++i) {
        const uint8_t* p = &firmware[words * 3 + i * 2];
        rom[i] = msbFirst ? static_cast<uint16_t>(p[0] << 8 | p[1]) : static_cast<uint16_t>(p[1] << 8 | p[0]);
    }
    loadProgram(model, map, msbFirst ? big : little, rom, hz);
    return true;
}

void NecDsp::loadProgram(Model model, Map map, const std::vector<uint32_t>& program, const std::vector<uint16_t>& data,
                         double hz) {
    setup(model, map, hz);
    program_.assign(static_cast<size_t>(pcMask_) + 1, 0);
    dataRom_.assign(static_cast<size_t>(rpMask_) + 1, 0);
    for (size_t i = 0; i < program.size() && i < program_.size(); ++i) program_[i] = program[i] & 0xFFFFFF;
    for (size_t i = 0; i < data.size() && i < dataRom_.size(); ++i) dataRom_[i] = data[i];
    power();
}

void NecDsp::power() {
    // The ST01x's data RAM is the game's battery save: switching off keeps it.
    if (map_ != Map::St01x) dataRam_.fill(0);
    reset();
}

void NecDsp::reset() {
    pc_ = rp_ = dp_ = 0;
    sp_ = 0;
    stack_.fill(0);
    a_ = b_ = tr_ = trb_ = k_ = l_ = m_ = n_ = 0;
    dr_ = 0;
    sr_ = 0;
    so_ = si_ = 0;
    flagA_ = flagB_ = Flags{};
    budget_ = 0;
}

void NecDsp::catchUp() {
    if (program_.empty()) return;
    while (budget_ >= masterHz_) {
        step();
        budget_ -= masterHz_;
    }
}

void NecDsp::step() {
    const uint32_t opcode = program_[pc_];
    pc_ = static_cast<uint16_t>((pc_ + 1) & pcMask_);
    switch (opcode >> 22) {
        case 0: op(opcode); break;
        case 1:  // RT: an operation, then return
            op(opcode);
            pc_ = static_cast<uint16_t>(pop() & pcMask_);
            break;
        case 2: jump(opcode); break;
        case 3: ld(static_cast<uint16_t>(opcode >> 6), static_cast<int>(opcode & 15)); break;
    }
    // The multiplier: K times L, sign and the top 15 bits in M, the low 15 bits in N.
    const int32_t product = static_cast<int16_t>(k_) * static_cast<int16_t>(l_);
    m_ = static_cast<uint16_t>(product >> 15);
    n_ = static_cast<uint16_t>(product << 1);
}

void NecDsp::op(uint32_t opcode) {
    const int pselect = (opcode >> 20) & 3, alu = (opcode >> 16) & 15, asl = (opcode >> 15) & 1;
    const int dpl = (opcode >> 13) & 3, dphm = (opcode >> 9) & 15, rpdcr = (opcode >> 8) & 1;
    const int src = (opcode >> 4) & 15, dst = opcode & 15;

    uint16_t idb = 0;
    switch (src) {
        case 0: idb = trb_; break;
        case 1: idb = a_; break;
        case 2: idb = b_; break;
        case 3: idb = tr_; break;
        case 4: idb = dp_; break;
        case 5: idb = rp_; break;
        case 6: idb = dataRom_[rp_ & rpMask_]; break;
        case 7: idb = static_cast<uint16_t>(0x8000 - (flagA_.s1 ? 1 : 0)); break;  // SGN: the saturated value
        case 8:
            idb = dr_;
            sr_ |= kRqm;
            break;
        case 9: idb = dr_; break;
        case 10: idb = srValue(); break;
        case 11:
        case 12: idb = si_; break;
        case 13: idb = k_; break;
        case 14: idb = l_; break;
        case 15: idb = dataRam_[dp_ & dpMask_]; break;
    }

    if (alu) {
        uint16_t p = 0;
        switch (pselect) {
            case 0: p = dataRam_[dp_ & dpMask_]; break;
            case 1: p = idb; break;
            case 2: p = m_; break;
            case 3: p = n_; break;
        }
        uint16_t& acc = asl ? b_ : a_;
        Flags flag = asl ? flagB_ : flagA_;
        const bool carry = asl ? flagA_.c : flagB_.c;  // the other accumulator's carry
        const uint16_t q = acc;
        uint16_t r = 0;
        switch (alu) {
            case 1: r = q | p; break;
            case 2: r = q & p; break;
            case 3: r = q ^ p; break;
            case 4: r = static_cast<uint16_t>(q - p); break;
            case 5: r = static_cast<uint16_t>(q + p); break;
            case 6: r = static_cast<uint16_t>(q - p - carry); break;
            case 7: r = static_cast<uint16_t>(q + p + carry); break;
            case 8:
                r = static_cast<uint16_t>(q - 1);
                p = 1;
                break;
            case 9:
                r = static_cast<uint16_t>(q + 1);
                p = 1;
                break;
            case 10: r = static_cast<uint16_t>(~q); break;
            case 11: r = static_cast<uint16_t>((q >> 1) | (q & 0x8000)); break;
            case 12: r = static_cast<uint16_t>((q << 1) | (carry ? 1 : 0)); break;
            case 13: r = static_cast<uint16_t>((q << 2) | 3); break;
            case 14: r = static_cast<uint16_t>((q << 4) | 15); break;
            case 15: r = static_cast<uint16_t>((q << 8) | (q >> 8)); break;
        }
        flag.s0 = r & 0x8000;
        flag.z = r == 0;
        if (!flag.ov1) flag.s1 = flag.s0;
        switch (alu) {
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
                if (alu & 1) {
                    flag.ov0 = (q ^ r) & (p ^ r) & 0x8000;
                    flag.c = r < q || (alu == 7 && carry && r == q);
                } else {
                    flag.ov0 = (q ^ r) & (q ^ p) & 0x8000;
                    flag.c = r > q || (alu == 6 && carry && r == q);
                }
                // OV1 says the result overflowed and has not come back: a second
                // overflow the other way undoes the first.
                flag.ov1 = flag.ov0 && flag.ov1 ? flag.s1 == flag.s0 : flag.ov0 || flag.ov1;
                break;
            case 11:
                flag.c = q & 1;
                flag.ov0 = flag.ov1 = false;
                break;
            case 12:
                flag.c = q >> 15;
                flag.ov0 = flag.ov1 = false;
                break;
            default:
                flag.c = false;
                flag.ov0 = flag.ov1 = false;
                break;
        }
        acc = r;
        (asl ? flagB_ : flagA_) = flag;
    }

    ld(idb, dst);

    if (dst != 4) {
        switch (dpl) {
            case 1: dp_ = static_cast<uint16_t>((dp_ & 0xF0) | ((dp_ + 1) & 0x0F)); break;
            case 2: dp_ = static_cast<uint16_t>((dp_ & 0xF0) | ((dp_ - 1) & 0x0F)); break;
            case 3: dp_ = static_cast<uint16_t>(dp_ & 0xF0); break;
            default: break;
        }
        dp_ = static_cast<uint16_t>((dp_ ^ (dphm << 4)) & dpMask_);
    }
    if (dst != 5 && rpdcr) rp_ = static_cast<uint16_t>((rp_ - 1) & rpMask_);
}

void NecDsp::ld(uint16_t id, int dst) {
    switch (dst) {
        case 0: break;
        case 1: a_ = id; break;
        case 2: b_ = id; break;
        case 3: tr_ = id; break;
        case 4: dp_ = id & dpMask_; break;
        case 5: rp_ = id & rpMask_; break;
        case 6:
            dr_ = id;
            sr_ |= kRqm;
            break;
        case 7: sr_ = static_cast<uint16_t>((sr_ & 0x907C) | (id & ~0x907C)); break;
        case 8: {  // SO, least significant bit first
            uint16_t r = 0;
            for (int i = 0; i < 16; ++i)
                if (id & (1 << i)) r |= static_cast<uint16_t>(0x8000 >> i);
            so_ = r;
            break;
        }
        case 9: so_ = id; break;
        case 10: k_ = id; break;
        case 11:
            k_ = id;
            l_ = dataRom_[rp_ & rpMask_];
            break;
        case 12:
            l_ = id;
            k_ = dataRam_[(dp_ | 0x40) & dpMask_];
            break;
        case 13: l_ = id; break;
        case 14: trb_ = id; break;
        case 15: dataRam_[dp_ & dpMask_] = id; break;
    }
}

void NecDsp::jump(uint32_t opcode) {
    const uint32_t brch = (opcode >> 13) & 0x1FF;
    uint16_t to;
    if (model_ == Model::Upd7725) {
        to = static_cast<uint16_t>((opcode >> 2) & 0x7FF);
    } else {
        // Eleven bits and a two-bit bank within the current half of the program.
        to = static_cast<uint16_t>((pc_ & 0x2000) | (opcode & 3) << 11 | ((opcode >> 2) & 0x7FF));
    }
    bool take = false;
    switch (brch) {
        case 0x000:
            pc_ = static_cast<uint16_t>(so_ & pcMask_);
            return;
        case 0x080: take = !flagA_.c; break;
        case 0x082: take = flagA_.c; break;
        case 0x084: take = !flagB_.c; break;
        case 0x086: take = flagB_.c; break;
        case 0x088: take = !flagA_.z; break;
        case 0x08A: take = flagA_.z; break;
        case 0x08C: take = !flagB_.z; break;
        case 0x08E: take = flagB_.z; break;
        case 0x090: take = !flagA_.ov0; break;
        case 0x092: take = flagA_.ov0; break;
        case 0x094: take = !flagB_.ov0; break;
        case 0x096: take = flagB_.ov0; break;
        case 0x098: take = !flagA_.ov1; break;
        case 0x09A: take = flagA_.ov1; break;
        case 0x09C: take = !flagB_.ov1; break;
        case 0x09E: take = flagB_.ov1; break;
        case 0x0A0: take = !flagA_.s0; break;
        case 0x0A2: take = flagA_.s0; break;
        case 0x0A4: take = !flagB_.s0; break;
        case 0x0A6: take = flagB_.s0; break;
        case 0x0A8: take = !flagA_.s1; break;
        case 0x0AA: take = flagA_.s1; break;
        case 0x0AC: take = !flagB_.s1; break;
        case 0x0AE: take = flagB_.s1; break;
        case 0x0B0: take = (dp_ & 0x0F) == 0x00; break;
        case 0x0B1: take = (dp_ & 0x0F) != 0x00; break;
        case 0x0B2: take = (dp_ & 0x0F) == 0x0F; break;
        case 0x0B3: take = (dp_ & 0x0F) != 0x0F; break;
        // The serial port's acknowledges: nothing is wired to it, so never.
        case 0x0B4:
        case 0x0B8: take = true; break;
        case 0x0B6:
        case 0x0BA: take = false; break;
        case 0x0BC: take = !(sr_ & kRqm); break;
        case 0x0BE: take = sr_ & kRqm; break;
        case 0x100:
            pc_ = static_cast<uint16_t>(to & ~0x2000 & pcMask_);
            return;
        case 0x101:
            pc_ = static_cast<uint16_t>((to | 0x2000) & pcMask_);
            return;
        case 0x140:
            push(pc_);
            pc_ = static_cast<uint16_t>(to & ~0x2000 & pcMask_);
            return;
        case 0x141:
            push(pc_);
            pc_ = static_cast<uint16_t>((to | 0x2000) & pcMask_);
            return;
        default: return;
    }
    if (take) pc_ = static_cast<uint16_t>(to & pcMask_);
}

// The host's side: DR one byte at a time (both bytes, low first, unless SR says 8-bit),
// RQM set while the chip waits for the host.
uint8_t NecDsp::readDr() {
    if (sr_ & kDrc) {
        sr_ &= ~kRqm;
        return static_cast<uint8_t>(dr_);
    }
    if (!(sr_ & kDrs)) {
        sr_ |= kDrs;
        return static_cast<uint8_t>(dr_);
    }
    sr_ &= ~(kRqm | kDrs);
    return static_cast<uint8_t>(dr_ >> 8);
}

void NecDsp::writeDr(uint8_t value) {
    if (sr_ & kDrc) {
        sr_ &= ~kRqm;
        dr_ = static_cast<uint16_t>((dr_ & 0xFF00) | value);
        return;
    }
    if (!(sr_ & kDrs)) {
        sr_ |= kDrs;
        dr_ = static_cast<uint16_t>((dr_ & 0xFF00) | value);
        return;
    }
    sr_ &= ~(kRqm | kDrs);
    dr_ = static_cast<uint16_t>((dr_ & 0x00FF) | value << 8);
}

namespace {

// Which register an address reaches: 0 DR, 1 SR, 2 the data RAM (ST01x), -1 none.
int decode(NecDsp::Map map, uint32_t address) {
    const uint32_t bank = (address >> 16) & 0x7F, a = address & 0xFFFF;
    switch (map) {
        case NecDsp::Map::LoRom:
        case NecDsp::Map::LoRomRam:
            if (bank >= (map == NecDsp::Map::LoRom ? 0x30u : 0x20u) && bank < 0x40 && a >= 0x8000) return a < 0xC000 ? 0 : 1;
            return -1;
        case NecDsp::Map::LoRomHigh:
            if (bank >= 0x60 && bank < 0x70 && a < 0x8000) return a < 0x4000 ? 0 : 1;
            return -1;
        case NecDsp::Map::HiRom:
            if (bank < 0x20 && a >= 0x6000 && a < 0x8000) return a < 0x7000 ? 0 : 1;
            return -1;
        case NecDsp::Map::St01x:
            if (bank >= 0x60 && bank < 0x68 && a < 0x4000) return a & 1;
            if (bank >= 0x68 && bank < 0x70 && a < 0x8000) return 2;
            return -1;
    }
    return -1;
}

}  // namespace

bool NecDsp::read(uint32_t address, uint8_t& value) {
    const int reg = decode(map_, address);
    if (reg < 0) return false;
    catchUp();
    if (reg == 0) value = readDr();
    else if (reg == 1) value = readSr();
    else {
        const uint32_t o = address & 0x0FFF;
        const uint16_t w = dataRam_[(o >> 1) & dpMask_];
        value = static_cast<uint8_t>(o & 1 ? w >> 8 : w);
    }
    return true;
}

bool NecDsp::write(uint32_t address, uint8_t value) {
    const int reg = decode(map_, address);
    if (reg < 0) return false;
    catchUp();
    if (reg == 0) {
        writeDr(value);
    } else if (reg == 2) {
        const uint32_t o = address & 0x0FFF;
        uint16_t& w = dataRam_[(o >> 1) & dpMask_];
        const uint16_t before = w;
        w = o & 1 ? static_cast<uint16_t>((w & 0x00FF) | value << 8) : static_cast<uint16_t>((w & 0xFF00) | value);
        if (w != before) batteryChanged_ = true;
    }
    return true;
}

std::vector<uint8_t> NecDsp::batteryData() const {
    if (map_ != Map::St01x) return {};
    std::vector<uint8_t> out(0x1000);
    for (size_t i = 0; i < 0x800; ++i) {
        out[i * 2] = static_cast<uint8_t>(dataRam_[i]);
        out[i * 2 + 1] = static_cast<uint8_t>(dataRam_[i] >> 8);
    }
    return out;
}

void NecDsp::loadBatteryData(const std::vector<uint8_t>& data) {
    if (map_ != Map::St01x) return;
    for (size_t i = 0; i < 0x800 && i * 2 + 1 < data.size(); ++i)
        dataRam_[i] = static_cast<uint16_t>(data[i * 2] | data[i * 2 + 1] << 8);
}

}  // namespace plumnin::snes
