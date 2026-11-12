#pragma once

#include <cstdint>

namespace plumnin {

// The buttons of one pad, true while held: the Super Nintendo's, which has them all.
// The Game Boy and the NES only read the cross, A, B, Select and Start.
struct PadState {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false, select = false, start = false;
};

// What is plugged into a port: a pad or nothing. A console with no second port (the
// Game Boy) ignores the second.
enum class PadType : uint8_t { Pad = 0, None = 1 };
constexpr int kPadTypes = 2;

}  // namespace plumnin
