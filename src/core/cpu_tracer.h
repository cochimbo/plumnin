#pragma once

#include <cstdint>

namespace plumnin {

// Sacar canciones (songs.cpp) watches a console's processor through a tracer, and runs
// pieces of the game's code on their own.
class CpuTracer {
public:
    virtual ~CpuTracer() = default;
    // Before each instruction.
    virtual void instruction(uint16_t pc, uint16_t sp) = 0;
    // A call taken, or an interrupt served: `to` is where it went, `sp` the stack with the
    // return address on it (on the NES, 0100 plus the stack pointer).
    virtual void call(uint16_t to, uint16_t sp, bool interrupt) = 0;
    // A read or write by the processor.
    virtual void access(uint16_t address, uint8_t value, bool write) = 0;
};

}  // namespace plumnin
