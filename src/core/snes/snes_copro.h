#pragma once

#include <cstdint>
#include <vector>

#include "../state_io.h"

namespace plumnin::snes {

// A chip on the cartridge beside the ROM (the DSP-1, the Super FX...), as the console
// sees it: some addresses of the processor's bus, and time. It runs behind the
// processor on its own clock and catches up whenever it is touched, and at the end of
// each frame.
class Coprocessor {
public:
    virtual ~Coprocessor() = default;
    virtual void power() = 0;
    virtual void reset() = 0;
    // Its answer at that address of the bus (true), or false where it is not.
    virtual bool read(uint32_t address, uint8_t& value) = 0;
    virtual bool write(uint32_t address, uint8_t value) = 0;
    // Master clocks passed on the processor's side, and the master clock's rate.
    virtual void addTime(uint32_t clocks) = 0;
    virtual void setMasterRate(double hz) = 0;
    virtual void catchUp() = 0;
    // The address the console's processor is about to reach (for chips that share the
    // bus with it and wait when both want the same memory).
    virtual void cpuAccess(uint32_t) {}
    // Its interrupt request to the processor.
    virtual bool irq() const { return false; }

    virtual void save(StateWriter& w) = 0;
    virtual void load(StateReader& r) = 0;

    // Memory of its own kept by the cartridge's battery (the ST010's), empty if none.
    virtual std::vector<uint8_t> batteryData() const { return {}; }
    virtual void loadBatteryData(const std::vector<uint8_t>&) {}
    bool batteryChanged() const { return batteryChanged_; }
    void clearBatteryChanged() { batteryChanged_ = false; }

protected:
    bool batteryChanged_ = false;
};

}  // namespace plumnin::snes
