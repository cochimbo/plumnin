#include "snes_cx4.h"

#include <type_traits>
#include <utility>

namespace plumnin::snes {

// Checked against ares' HG51B and its Hitachi DSP board.

namespace {

constexpr uint32_t kMask = 0xFFFFFF;
constexpr uint8_t kShifts[4] = {0, 1, 8, 16};

uint32_t signExtend24(uint32_t v) { return v & 0x800000 ? v | 0xFF000000u : v; }

}  // namespace

bool Cx4::setFirmware(const std::vector<uint8_t>& firmware) {
    if (firmware.size() != 3072) return false;
    for (size_t i = 0; i < 1024; ++i)
        dataRom_[i] = firmware[i * 3] | firmware[i * 3 + 1] << 8 | static_cast<uint32_t>(firmware[i * 3 + 2]) << 16;
    return true;
}

void Cx4::power() {
    r_ = {};
    io_ = {};
    programRam_ = {};
    dataRam_.fill(0);
    stack_.fill(0);
    irqLine_ = false;
    budget_ = 0;
}

// The console's side.

uint8_t Cx4::romRead(uint32_t address) const {
    if (rom_.empty()) return 0;
    const uint32_t offset = ((address & 0x3F0000) >> 1 | (address & 0x7FFF)) & 0x1FFFFF;
    return rom_[offset % rom_.size()];
}

bool Cx4::read(uint32_t address, uint8_t& value) {
    if (isRom(address)) {
        catchUp();
        // While the chip has the ROM, the console sees the vectors from its registers.
        if (busy() && (address & 0xBFFFC0) == 0x007FC0) value = ioRead(static_cast<uint16_t>(0x7F40 | (address & 0x3F)));
        else value = romRead(address);  // (the board gives open bus otherwise; the ROM is kinder)
        return true;
    }
    if (isRam(address)) {
        value = ram_[(((address & 0x070000) >> 1) | (address & 0x7FFF)) % ram_.size()];
        return true;
    }
    if (isDataRam(address)) {
        catchUp();
        const uint32_t a = address & 0xFFF;
        if (a >= 0xC00) return false;
        value = dataRam_[a];
        return true;
    }
    if (isIo(address)) {
        catchUp();
        value = ioRead(static_cast<uint16_t>(0x7C00 | (address & 0x3FF)));
        return true;
    }
    return false;
}

bool Cx4::write(uint32_t address, uint8_t value) {
    if (isRom(address)) return true;
    if (isRam(address)) {
        uint8_t& b = ram_[(((address & 0x070000) >> 1) | (address & 0x7FFF)) % ram_.size()];
        if (b != value) batteryChanged_ = true;
        b = value;
        return true;
    }
    if (isDataRam(address)) {
        catchUp();
        const uint32_t a = address & 0xFFF;
        if (a < 0xC00) dataRam_[a] = value;
        return true;
    }
    if (isIo(address)) {
        catchUp();
        ioWrite(static_cast<uint16_t>(0x7C00 | (address & 0x3FF)), value);
        return true;
    }
    return false;
}

uint8_t Cx4::ioRead(uint16_t a) const {
    auto byte = [](uint32_t v, int n) { return static_cast<uint8_t>(v >> (8 * n)); };
    switch (a) {
        case 0x7F40: case 0x7F41: case 0x7F42: return byte(io_.dmaSource, a - 0x7F40);
        case 0x7F43: case 0x7F44: return byte(io_.dmaLength, a - 0x7F43);
        case 0x7F45: case 0x7F46: case 0x7F47: return byte(io_.dmaTarget, a - 0x7F45);
        case 0x7F48: return io_.cachePage;
        case 0x7F49: case 0x7F4A: case 0x7F4B: return byte(io_.cacheBase, a - 0x7F49);
        case 0x7F4C: return static_cast<uint8_t>(io_.cacheLock[0] | io_.cacheLock[1] << 1);
        case 0x7F4D: case 0x7F4E: return byte(io_.cachePb, a - 0x7F4D);
        case 0x7F4F: return io_.cachePc;
        case 0x7F50: return static_cast<uint8_t>(io_.waitRam | io_.waitRom << 4);
        case 0x7F51: return io_.irq;
        case 0x7F52: return io_.rom;
        case 0x7F53: case 0x7F54: case 0x7F55: case 0x7F56: case 0x7F57: case 0x7F59: case 0x7F5B: case 0x7F5C:
        case 0x7F5D: case 0x7F5E: case 0x7F5F: {
            const bool running = busy() || !io_.halt;
            return static_cast<uint8_t>(io_.suspend | r_.i << 1 | running << 6 | busy() << 7);
        }
        default: break;
    }
    if (a >= 0x7F60 && a <= 0x7F7F) return io_.vector[a & 0x1F];
    if ((a >= 0x7F80 && a <= 0x7FAF) || (a >= 0x7FC0 && a <= 0x7FEF)) {
        const int n = a & 0x3F;
        return byte(r_.gpr[static_cast<size_t>(n / 3)], n % 3);
    }
    return 0;
}

void Cx4::ioWrite(uint16_t a, uint8_t value) {
    auto setByte = [value](auto& v, int n) {
        using T = std::remove_reference_t<decltype(v)>;
        v = static_cast<T>((v & ~(static_cast<T>(0xFF) << (8 * n))) | static_cast<T>(value) << (8 * n));
    };
    switch (a) {
        case 0x7F40: case 0x7F41: case 0x7F42: setByte(io_.dmaSource, a - 0x7F40); return;
        case 0x7F43: case 0x7F44: setByte(io_.dmaLength, a - 0x7F43); return;
        case 0x7F45: case 0x7F46: setByte(io_.dmaTarget, a - 0x7F45); return;
        case 0x7F47:
            setByte(io_.dmaTarget, 2);
            if (io_.halt) io_.dmaEnable = true;
            return;
        case 0x7F48:
            io_.cachePage = value & 1;
            if (io_.halt) {
                r_.pb = io_.cachePb;
                io_.preload = true;
                io_.cacheEnable = true;
            }
            return;
        case 0x7F49: case 0x7F4A: case 0x7F4B: setByte(io_.cacheBase, a - 0x7F49); return;
        case 0x7F4C:
            io_.cacheLock[0] = value & 1;
            io_.cacheLock[1] = value & 2;
            return;
        case 0x7F4D: case 0x7F4E:
            setByte(io_.cachePb, a - 0x7F4D);
            io_.cachePb &= 0x7FFF;
            return;
        case 0x7F4F:
            io_.cachePc = value;
            if (io_.halt) {  // start
                io_.halt = false;
                r_.pb = io_.cachePb;
                r_.pc = io_.cachePc;
            }
            return;
        case 0x7F50:
            io_.waitRam = value & 7;
            io_.waitRom = (value >> 4) & 7;
            return;
        case 0x7F51:
            io_.irq = value & 1;
            if (io_.irq) irqLine_ = r_.i = false;
            return;
        case 0x7F52: io_.rom = value & 1; return;
        case 0x7F53:
            io_.lock = false;
            io_.halt = true;
            return;
        case 0x7F55: case 0x7F56: case 0x7F57: case 0x7F58: case 0x7F59: case 0x7F5A: case 0x7F5B: case 0x7F5C:
            io_.suspend = true;
            io_.suspendFor = static_cast<uint8_t>((a - 0x7F55) * 32);  // 0: until resumed
            return;
        case 0x7F5D: io_.suspend = false; return;
        case 0x7F5E: r_.i = false; return;  // (the line to the processor stays as it is)
        default: break;
    }
    if (a >= 0x7F60 && a <= 0x7F7F) {
        io_.vector[a & 0x1F] = value;
        return;
    }
    if ((a >= 0x7F80 && a <= 0x7FAF) || (a >= 0x7FC0 && a <= 0x7FEF)) {
        const int n = a & 0x3F;
        setByte(r_.gpr[static_cast<size_t>(n / 3)], n % 3);
    }
}

// The chip's side.

uint8_t Cx4::busRead(uint32_t address) {
    address &= kMask;
    if (isRom(address)) return romRead(address);
    if (isRam(address)) return ram_[(((address & 0x070000) >> 1) | (address & 0x7FFF)) % ram_.size()];
    if (isDataRam(address)) return (address & 0xFFF) < 0xC00 ? dataRam_[address & 0xFFF] : 0;
    if (isIo(address)) return ioRead(static_cast<uint16_t>(0x7C00 | (address & 0x3FF)));
    return 0;
}

void Cx4::busWrite(uint32_t address, uint8_t value) {
    address &= kMask;
    if (isRom(address)) return;
    if (isRam(address)) {
        uint8_t& b = ram_[(((address & 0x070000) >> 1) | (address & 0x7FFF)) % ram_.size()];
        if (b != value) batteryChanged_ = true;
        b = value;
    } else if (isDataRam(address)) {
        if ((address & 0xFFF) < 0xC00) dataRam_[address & 0xFFF] = value;
    } else if (isIo(address)) {
        ioWrite(static_cast<uint16_t>(0x7C00 | (address & 0x3FF)), value);
    }
}

uint32_t Cx4::wait(uint32_t address) const {
    if (isRom(address)) return 1u + io_.waitRom;
    if (isRam(address)) return 1u + io_.waitRam;
    return 0;
}

// Time passes, and a read or write it asked for on the bus ends.
void Cx4::stepCycles(uint32_t cycles) {
    budget_ -= static_cast<int64_t>(cycles) * masterHz_;
    if (!io_.busEnable) return;
    if (io_.busPending > cycles) {
        io_.busPending = static_cast<uint8_t>(io_.busPending - cycles);
        return;
    }
    io_.busEnable = false;
    io_.busPending = 0;
    if (io_.busReading) {
        io_.busReading = false;
        r_.mdr = busRead(io_.busAddress);
    }
    if (io_.busWriting) {
        io_.busWriting = false;
        busWrite(io_.busAddress, static_cast<uint8_t>(r_.mdr));
    }
}

void Cx4::halt() {
    io_.halt = true;
    if (!io_.irq) irqLine_ = r_.i = true;
}

void Cx4::catchUp() {
    while (budget_ > 0) {
        // Waiting with nothing to do: no need to count the cycles one by one.
        const bool idle = io_.lock || (io_.suspend && !io_.suspendFor) ||
                          (io_.halt && !io_.cacheEnable && !io_.dmaEnable && !io_.suspend);
        if (idle && !io_.busEnable) {
            budget_ = 0;
            return;
        }
        runOne();
    }
}

void Cx4::runOne() {
    if (io_.lock) return stepCycles(1);
    if (io_.suspend) {
        if (!io_.suspendFor) return stepCycles(1);
        stepCycles(io_.suspendFor);
        io_.suspendFor = 0;
        io_.suspend = false;
        return;
    }
    if (io_.cacheEnable) {
        cache();
        return;
    }
    if (io_.dmaEnable) return dma();
    if (io_.halt) return stepCycles(1);
    execute();
}

void Cx4::execute() {
    if (!cache()) return halt();
    const uint16_t op = programRam_[io_.cachePage][r_.pc];
    advance();
    stepCycles(1);
    instruction(op);
}

void Cx4::advance() {
    if (++r_.pc == 0) {
        if (io_.cachePage) return halt();
        io_.cachePage = true;
        if (io_.cacheLock[1]) return halt();
        r_.pb = r_.p;
        if (!cache()) return halt();
    }
}

// The program's page in one of the two caches: if neither has it, it loads into one
// that isn't locked.
bool Cx4::cache() {
    uint32_t address = (io_.cacheBase + r_.pb * 512u) & kMask;
    if (!io_.preload) {
        if (io_.cacheAddress[io_.cachePage] == address) return io_.cacheEnable = false, true;
        io_.cachePage = !io_.cachePage;
        if (io_.cacheAddress[io_.cachePage] == address) return io_.cacheEnable = false, true;
        if (io_.cacheLock[io_.cachePage]) io_.cachePage = !io_.cachePage;
        if (io_.cacheLock[io_.cachePage]) return io_.cacheEnable = false, false;
    }
    io_.preload = false;
    io_.cacheAddress[io_.cachePage] = address;
    for (auto& word : programRam_[io_.cachePage]) {
        stepCycles(wait(address));
        const uint8_t lo = busRead(address++);
        stepCycles(wait(address));
        word = static_cast<uint16_t>(lo | busRead(address++) << 8);
    }
    io_.cacheEnable = false;
    return true;
}

void Cx4::dma() {
    for (uint32_t offset = 0; offset < io_.dmaLength; ++offset) {
        const uint32_t source = (io_.dmaSource + offset) & kMask, target = (io_.dmaTarget + offset) & kMask;
        if ((isRom(source) && isRom(target)) || (isRam(source) && isRam(target))) {
            io_.lock = true;
            return;
        }
        stepCycles(wait(source));
        const uint8_t v = busRead(source);
        stepCycles(wait(target));
        busWrite(target, v);
    }
    io_.dmaEnable = false;
}

uint32_t Cx4::readRegister(uint8_t reg) {
    switch (reg & 0x7F) {
        case 0x01: return static_cast<uint32_t>(r_.mul >> 24) & kMask;
        case 0x02: return static_cast<uint32_t>(r_.mul) & kMask;
        case 0x03: return r_.mdr;
        case 0x08: return r_.rom;
        case 0x0C: return r_.ram;
        case 0x13: return r_.mar;
        case 0x1C: return r_.dpr;
        case 0x20: return r_.pc;
        case 0x28: return r_.p;
        case 0x2E: case 0x2F:  // a read of the bus at MAR, which WAIT finishes
            io_.busEnable = io_.busReading = true;
            io_.busPending = (reg & 1) ? io_.waitRam : io_.waitRom;
            io_.busAddress = r_.mar;
            return 0;
        case 0x50: return 0x000000;
        case 0x51: return 0xFFFFFF;
        case 0x52: return 0x00FF00;
        case 0x53: return 0xFF0000;
        case 0x54: return 0x00FFFF;
        case 0x55: return 0xFFFF00;
        case 0x56: return 0x800000;
        case 0x57: return 0x7FFFFF;
        case 0x58: return 0x008000;
        case 0x59: return 0x007FFF;
        case 0x5A: return 0xFF7FFF;
        case 0x5B: return 0xFFFF7F;
        case 0x5C: return 0x010000;
        case 0x5D: return 0xFEFFFF;
        case 0x5E: return 0x000100;
        case 0x5F: return 0x00FEFF;
        default: break;
    }
    if ((reg & 0x60) == 0x60) return r_.gpr[reg & 15];
    return 0;
}

void Cx4::writeRegister(uint8_t reg, uint32_t value) {
    value &= kMask;
    switch (reg & 0x7F) {
        case 0x01: r_.mul = (r_.mul & 0xFFFFFF) | static_cast<uint64_t>(value) << 24; return;
        case 0x02: r_.mul = (r_.mul & 0xFFFFFF000000ull) | value; return;
        case 0x03: r_.mdr = value; return;
        case 0x08: r_.rom = value; return;
        case 0x0C: r_.ram = value; return;
        case 0x13: r_.mar = value; return;
        case 0x1C: r_.dpr = value; return;
        case 0x20: r_.pc = static_cast<uint8_t>(value); return;
        case 0x28: r_.p = value & 0x7FFF; return;
        case 0x2E: case 0x2F:  // a write of MDR to the bus at MAR
            io_.busEnable = io_.busWriting = true;
            io_.busPending = (reg & 1) ? io_.waitRam : io_.waitRom;
            io_.busAddress = r_.mar;
            return;
        default: break;
    }
    if ((reg & 0x60) == 0x60) r_.gpr[reg & 15] = value;
}

void Cx4::push() {
    for (size_t i = 7; i > 0; --i) stack_[i] = stack_[i - 1];
    stack_[0] = static_cast<uint32_t>(r_.pb) << 8 | r_.pc;
}

void Cx4::pull() {
    const uint32_t top = stack_[0];
    for (size_t i = 0; i < 7; ++i) stack_[i] = stack_[i + 1];
    stack_[7] = 0;
    r_.pb = (top >> 8) & 0x7FFF;
    r_.pc = static_cast<uint8_t>(top);
}

uint32_t Cx4::add(uint32_t x, uint32_t y) {
    x &= kMask;
    y &= kMask;
    const uint32_t z = x + y;
    r_.n = z & 0x800000;
    r_.z = (z & kMask) == 0;
    r_.c = z > kMask;
    r_.v = ~(x ^ y) & (x ^ z) & 0x800000;
    return z & kMask;
}

uint32_t Cx4::sub(uint32_t x, uint32_t y) {
    x &= kMask;
    y &= kMask;
    const int32_t z = static_cast<int32_t>(x) - static_cast<int32_t>(y);
    const uint32_t u = static_cast<uint32_t>(z);
    r_.n = u & 0x800000;
    r_.z = (u & kMask) == 0;
    r_.c = z >= 0;
    r_.v = ~(x ^ y) & (x ^ u) & 0x800000;
    return u & kMask;
}

uint32_t Cx4::logic(uint32_t x) {
    x &= kMask;
    r_.n = x & 0x800000;
    r_.z = x == 0;
    return x;
}

void Cx4::rdram(int byte, uint32_t address) {
    address &= 0xFFF;
    if (address >= 0xC00) address -= 0x400;
    r_.ram = (r_.ram & ~(0xFFu << (8 * byte))) | static_cast<uint32_t>(dataRam_[address]) << (8 * byte);
}

void Cx4::wrram(int byte, uint32_t address) {
    address &= 0xFFF;
    if (address >= 0xC00) address -= 0x400;
    dataRam_[address] = static_cast<uint8_t>(r_.ram >> (8 * byte));
}

void Cx4::instruction(uint16_t op) {
    const uint8_t imm = static_cast<uint8_t>(op);
    const uint8_t reg = op & 0x7F;
    const uint8_t sub2 = (op >> 8) & 3;
    const uint32_t shifted = (r_.a << kShifts[sub2]) & kMask;
    auto jump = [&](bool take, bool call) {
        if (!take) return;
        if (call) push();
        if (op & 0x200) r_.pb = r_.p;  // far
        r_.pc = imm;
        stepCycles(2);
    };
    auto skip = [&](bool flag) {
        if (flag != (op & 1)) return;
        advance();
        stepCycles(1);
    };
    // The shift amount of SHR, ASR, ROR and SHL: 5 bits, more than 24 is none.
    auto amount = [&](bool fromReg) {
        uint32_t s = (fromReg ? readRegister(reg) : op) & 0x1F;
        return s > 24 ? 0u : s;
    };
    auto mul = [&](uint32_t y) {
        const int64_t p = static_cast<int64_t>(static_cast<int32_t>(signExtend24(r_.a))) *
                          static_cast<int32_t>(signExtend24(y & kMask));
        r_.mul = static_cast<uint64_t>(p) & 0xFFFFFFFFFFFFull;
    };
    switch (op >> 10) {
        case 0x02: jump(true, false); break;
        case 0x03: jump(r_.z, false); break;
        case 0x04: jump(r_.c, false); break;
        case 0x05: jump(r_.n, false); break;
        case 0x06: jump(r_.v, false); break;
        case 0x07:  // WAIT: for the bus
            if (io_.busEnable) stepCycles(io_.busPending);
            break;
        case 0x09:
            switch (sub2) {
                case 0: skip(r_.v); break;
                case 1: skip(r_.c); break;
                case 2: skip(r_.z); break;
                default: skip(r_.n); break;
            }
            break;
        case 0x0A: jump(true, true); break;
        case 0x0B: jump(r_.z, true); break;
        case 0x0C: jump(r_.c, true); break;
        case 0x0D: jump(r_.n, true); break;
        case 0x0E: jump(r_.v, true); break;
        case 0x0F:  // RTS
            pull();
            stepCycles(2);
            break;
        case 0x10: r_.mar = (r_.mar + 1) & kMask; break;
        case 0x12: sub(readRegister(reg), shifted); break;  // CMPR
        case 0x13: sub(imm, shifted); break;
        case 0x14: sub(shifted, readRegister(reg)); break;  // CMP
        case 0x15: sub(shifted, imm); break;
        case 0x16:
            if (sub2 == 1) r_.a = logic(static_cast<uint32_t>(static_cast<int8_t>(r_.a)));    // SXB
            if (sub2 == 2) r_.a = logic(static_cast<uint32_t>(static_cast<int16_t>(r_.a)));   // SXW
            break;
        case 0x18:  // LD from a register
            switch (sub2) {
                case 0: r_.a = readRegister(reg); break;
                case 1: r_.mdr = readRegister(reg); break;
                case 2: r_.mar = readRegister(reg); break;
                default: r_.p = r_.gpr[op & 15] & 0x7FFF; break;
            }
            break;
        case 0x19:  // LD an immediate
            switch (sub2) {
                case 0: r_.a = imm; break;
                case 1: r_.mdr = imm; break;
                case 2: r_.mar = imm; break;
                default: r_.p = imm; break;
            }
            break;
        case 0x1A:
            if (sub2 != 3) rdram(sub2, r_.a);
            break;
        case 0x1B:
            if (sub2 != 3) rdram(sub2, r_.dpr + imm);
            break;
        case 0x1C: r_.rom = dataRom_[r_.a & 0x3FF]; break;
        case 0x1D: r_.rom = dataRom_[op & 0x3FF]; break;
        case 0x1F:
            if (sub2 == 0) r_.p = static_cast<uint16_t>((r_.p & 0x7F00) | imm);
            if (sub2 == 1) r_.p = static_cast<uint16_t>((r_.p & 0x00FF) | (imm & 0x7F) << 8);
            break;
        case 0x20: r_.a = add(shifted, readRegister(reg)); break;
        case 0x21: r_.a = add(shifted, imm); break;
        case 0x22: r_.a = sub(readRegister(reg), shifted); break;  // SUBR
        case 0x23: r_.a = sub(imm, shifted); break;
        case 0x24: r_.a = sub(shifted, readRegister(reg)); break;
        case 0x25: r_.a = sub(shifted, imm); break;
        case 0x26: mul(readRegister(reg)); break;
        case 0x27: mul(imm); break;
        case 0x28: r_.a = logic(~shifted ^ readRegister(reg)); break;
        case 0x29: r_.a = logic(~shifted ^ imm); break;
        case 0x2A: r_.a = logic(shifted ^ readRegister(reg)); break;
        case 0x2B: r_.a = logic(shifted ^ imm); break;
        case 0x2C: r_.a = logic(shifted & readRegister(reg)); break;
        case 0x2D: r_.a = logic(shifted & imm); break;
        case 0x2E: r_.a = logic(shifted | readRegister(reg)); break;
        case 0x2F: r_.a = logic(shifted | imm); break;
        case 0x30: case 0x31: r_.a = logic(r_.a >> amount(!(op & 0x400))); break;  // SHR
        case 0x32: case 0x33:                                                     // ASR
            r_.a = logic(static_cast<uint32_t>(static_cast<int32_t>(signExtend24(r_.a)) >> amount(!(op & 0x400))));
            break;
        case 0x34: case 0x35: {  // ROR
            const uint32_t s = amount(!(op & 0x400));
            r_.a = logic(r_.a >> s | r_.a << (24 - s));
            break;
        }
        case 0x36: case 0x37: r_.a = logic(r_.a << amount(!(op & 0x400))); break;  // SHL
        case 0x38:
            if (sub2 == 0) writeRegister(reg, r_.a);
            if (sub2 == 1) writeRegister(reg, r_.mdr);
            break;
        case 0x3A:
            if (sub2 != 3) wrram(sub2, r_.a);
            break;
        case 0x3B:
            if (sub2 != 3) wrram(sub2, r_.dpr + imm);
            break;
        case 0x3C: {  // SWAP
            uint32_t& g = r_.gpr[op & 15];
            std::swap(r_.a, g);
            break;
        }
        case 0x3E:  // CLEAR
            r_.a = 0;
            r_.p = 0;
            r_.ram = 0;
            r_.dpr = 0;
            break;
        case 0x3F: halt(); break;
        default: break;  // NOP, and the unused codes
    }
}

}  // namespace plumnin::snes
