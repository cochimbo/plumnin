#include "snes_obc1.h"

namespace plumnin::snes {

// 00-3F and 80-BF:6000-7FFF, and 70-71, F0-F1:6000-7FFF and E000-FFFF.
bool Obc1::mapped(uint32_t address) {
    const uint32_t bank = (address >> 16) & 0x7F, a = address & 0xFFFF;
    if (bank < 0x40) return a >= 0x6000 && a < 0x8000;
    if (bank == 0x70 || bank == 0x71) return (a & 0x6000) == 0x6000;
    return false;
}

void Obc1::setRam(uint16_t a, uint8_t value) {
    uint8_t& b = ram_[a % ram_.size()];
    if (b != value) batteryChanged_ = true;
    b = value;
}

bool Obc1::read(uint32_t address, uint8_t& value) {
    if (ram_.empty() || !mapped(address)) return false;
    const uint16_t a = address & 0x1FFF;
    if (a >= 0x1FF0 && a <= 0x1FF3) value = ram(static_cast<uint16_t>(base() + (index() << 2) + (a & 3)));
    else if (a == 0x1FF4) value = ram(static_cast<uint16_t>(base() + (index() >> 2) + 0x200));
    else value = ram(a);
    return true;
}

bool Obc1::write(uint32_t address, uint8_t value) {
    if (ram_.empty() || !mapped(address)) return false;
    const uint16_t a = address & 0x1FFF;
    if (a >= 0x1FF0 && a <= 0x1FF3) {
        setRam(static_cast<uint16_t>(base() + (index() << 2) + (a & 3)), value);
    } else if (a == 0x1FF4) {
        // The entry's two bits, among the four entries that share this byte.
        const uint16_t at = static_cast<uint16_t>(base() + (index() >> 2) + 0x200);
        const int shift = (ram(0x1FF6) & 3) << 1;
        setRam(at, static_cast<uint8_t>((ram(at) & ~(3 << shift)) | ((value & 3) << shift)));
    } else {
        setRam(a, value);
    }
    return true;
}

}  // namespace plumnin::snes
