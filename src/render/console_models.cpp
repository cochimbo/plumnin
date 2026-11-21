#include "console_models.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace plumnin {

namespace {

constexpr float kPi = 3.14159265f;

struct Colour {
    float r, g, b;
};

// The plastics and the rest, as their real colours look under the scene's light.
constexpr Colour kBlack{0.045f, 0.045f, 0.052f};
constexpr Colour kBlackSoft{0.065f, 0.065f, 0.075f};
constexpr Colour kHole{0.008f, 0.008f, 0.01f};
constexpr Colour kGroove{0.02f, 0.02f, 0.024f};
constexpr Colour kFlap{0.1f, 0.1f, 0.11f};
constexpr Colour kButton{0.12f, 0.12f, 0.135f};
constexpr Colour kGbGrey{0.62f, 0.61f, 0.57f};
constexpr Colour kGbBezel{0.24f, 0.24f, 0.29f};
constexpr Colour kGbMagenta{0.5f, 0.08f, 0.28f};
constexpr Colour kGbcTeal{0.08f, 0.5f, 0.5f};
constexpr Colour kNesLight{0.6f, 0.6f, 0.58f};
constexpr Colour kNesDark{0.26f, 0.26f, 0.28f};
constexpr Colour kSnesGrey{0.66f, 0.66f, 0.64f};
constexpr Colour kSnesDeck{0.45f, 0.45f, 0.47f};
constexpr Colour kSnesPurple{0.32f, 0.28f, 0.5f};

// Writes triangles in a frame that is either the world's (y up) or one turned to face
// the viewer, where the builder's y is the world's z (towards the viewer) and the
// builder's z is the world's -y; so the same shapes make the parts of a front.
class Builder {
public:
    explicit Builder(std::vector<float>& out) : out_(out) {}

    // Everything after this is moved by (x, y, z) and, if `front`, turned to face +z.
    void frame(float x, float y, float z, bool front) {
        ox_ = x, oy_ = y, oz_ = z;
        front_ = front;
    }

    // A rounded rectangle (w x d, corners of radius r) lofted through rings at heights
    // y, each pulled in by its inset: bevels, tapers and domes. Caps on the top ring and,
    // if asked, the bottom one.
    void loft(float cx, float cz, float w, float d, float r, std::initializer_list<std::pair<float, float>> rings,
              Colour c, bool bottom = false, int corner = 8) {
        const std::vector<std::pair<float, float>> ring(rings);
        corner = std::max(corner, 2);
        const int n = corner * 4;
        auto point = [&](size_t k, int i, float* px, float* pz, float* nx, float* nz) {
            const float inset = ring[k].second;
            const float hx = std::max(w / 2 - inset, 0.0005f), hz = std::max(d / 2 - inset, 0.0005f);
            const float rr = std::clamp(r - inset, 0.0005f, std::min(hx, hz));
            const int q = i / corner;
            const float a = (static_cast<float>(q) + static_cast<float>(i % corner) / static_cast<float>(corner - 1)) * kPi / 2;
            const float ccx = (q == 0 || q == 3) ? hx - rr : -(hx - rr), ccz = (q == 0 || q == 1) ? hz - rr : -(hz - rr);
            *nx = std::cos(a), *nz = std::sin(a);
            *px = cx + ccx + rr * *nx, *pz = cz + ccz + rr * *nz;
        };
        for (size_t k = 0; k + 1 < ring.size(); ++k) {
            const float y0 = ring[k].first, y1 = ring[k + 1].first, di = ring[k + 1].second - ring[k].second;
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                float ax, az, anx, anz, bx, bz, bnx, bnz, cx0, cz0, cx1, cz1, dummy;
                point(k, i, &ax, &az, &anx, &anz);
                point(k, j, &bx, &bz, &bnx, &bnz);
                point(k + 1, i, &cx0, &cz0, &dummy, &dummy);
                point(k + 1, j, &cx1, &cz1, &dummy, &dummy);
                // Outwards and up as much as the ring steps in.
                const float dy = y1 - y0;
                const float na[3] = {anx * dy, di, anz * dy}, nb[3] = {bnx * dy, di, bnz * dy};
                tri({ax, y0, az}, {bx, y0, bz}, {cx1, y1, cz1}, na, nb, nb, c);
                tri({ax, y0, az}, {cx1, y1, cz1}, {cx0, y1, cz0}, na, nb, na, c);
            }
        }
        cap(ring.size() - 1, n, point, ring.back().first, 1, c);
        if (bottom) cap(0, n, point, ring.front().first, -1, c);
    }

    // An upright box, w wide, d deep, from y0 to y1, its edges barely rounded.
    void box(float cx, float y0, float y1, float cz, float w, float d, Colour c) {
        loft(cx, cz, w, d, 0.004f, {{y0, 0}, {y1, 0}}, c, true, 2);
    }

    // A flat part on a surface facing up at height y: a slot, a button, a groove.
    void top(float cx, float cz, float y, float w, float d, float r, float raise, Colour c) {
        loft(cx, cz, w, d, r, {{y, 0}, {y + raise, std::min(raise * 0.3f, 0.004f)}}, c);
    }

    // The same on the front (facing +z at depth z): (cx, cy) is its middle.
    void front(float cx, float cy, float z, float w, float h, float r, float raise, Colour c) {
        const float x = ox_, y = oy_, zz = oz_;
        const bool f = front_;
        frame(x + cx, y + cy, zz + z, true);
        loft(0, 0, w, h, r, {{0, 0}, {raise, std::min(raise * 0.3f, 0.004f)}}, c);
        frame(x, y, zz, f);
    }

    // A prism along x from x0 to x1 whose cross-section is the convex polygon of (z, y)
    // points: the sloping parts of a body.
    void prismX(float x0, float x1, std::initializer_list<std::pair<float, float>> poly, Colour c) {
        const std::vector<std::pair<float, float>> p(poly);
        float mz = 0, my = 0;
        for (const auto& [z, y] : p) mz += z, my += y;
        mz /= static_cast<float>(p.size()), my /= static_cast<float>(p.size());
        for (size_t i = 0; i < p.size(); ++i) {
            const auto [z0, y0] = p[i];
            const auto [z1, y1] = p[(i + 1) % p.size()];
            float nz = y1 - y0, ny = -(z1 - z0);
            if (nz * ((z0 + z1) / 2 - mz) + ny * ((y0 + y1) / 2 - my) < 0) nz = -nz, ny = -ny;
            const float n[3] = {0, ny, nz};
            tri({x0, y0, z0}, {x1, y0, z0}, {x1, y1, z1}, n, n, n, c);
            tri({x0, y0, z0}, {x1, y1, z1}, {x0, y1, z1}, n, n, n, c);
            const float l[3] = {-1, 0, 0}, r[3] = {1, 0, 0};
            tri({x0, my, mz}, {x0, y0, z0}, {x0, y1, z1}, l, l, l, c);
            tri({x1, my, mz}, {x1, y0, z0}, {x1, y1, z1}, r, r, r, c);
        }
    }

    // A prism along z from z0 to z1 whose cross-section is the convex polygon of (x, y)
    // points: the outline of a cartridge.
    void prismZ(float z0, float z1, std::initializer_list<std::pair<float, float>> poly, Colour c) {
        const std::vector<std::pair<float, float>> p(poly);
        float mx = 0, my = 0;
        for (const auto& [x, y] : p) mx += x, my += y;
        mx /= static_cast<float>(p.size()), my /= static_cast<float>(p.size());
        for (size_t i = 0; i < p.size(); ++i) {
            const auto [x0, y0] = p[i];
            const auto [x1, y1] = p[(i + 1) % p.size()];
            float nx = y1 - y0, ny = -(x1 - x0);
            if (nx * ((x0 + x1) / 2 - mx) + ny * ((y0 + y1) / 2 - my) < 0) nx = -nx, ny = -ny;
            const float n[3] = {nx, ny, 0};
            tri({x0, y0, z0}, {x0, y0, z1}, {x1, y1, z1}, n, n, n, c);
            tri({x0, y0, z0}, {x1, y1, z1}, {x1, y1, z0}, n, n, n, c);
            const float back[3] = {0, 0, -1}, fwd[3] = {0, 0, 1};
            tri({mx, my, z0}, {x0, y0, z0}, {x1, y1, z0}, back, back, back, c);
            tri({mx, my, z1}, {x0, y0, z1}, {x1, y1, z1}, fwd, fwd, fwd, c);
        }
    }

    // A rectangle of a prism, from (x0, y0) to (x1, y1) and z0 to z1.
    void slab(float x0, float y0, float x1, float y1, float z0, float z1, Colour c) {
        prismZ(z0, z1, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, c);
    }

private:
    struct P {
        float x, y, z;
    };

    // A fan from the middle of ring k (of n points), facing up or down.
    template <typename Point>
    void cap(size_t k, int n, Point& point, float y, float up, Colour c) {
        const float normal[3] = {0, up, 0};
        std::vector<P> pts(static_cast<size_t>(n));
        P m{0, y, 0};
        for (int i = 0; i < n; ++i) {
            float px, pz, nx, nz;
            point(k, i, &px, &pz, &nx, &nz);
            pts[static_cast<size_t>(i)] = {px, y, pz};
            m.x += px, m.z += pz;
        }
        m.x /= static_cast<float>(n), m.z /= static_cast<float>(n);
        for (size_t i = 0; i < pts.size(); ++i) tri(m, pts[i], pts[(i + 1) % pts.size()], normal, normal, normal, c);
    }

    void tri(P a, P b, P c, const float* na, const float* nb, const float* nc, Colour col) {
        vertex(a, na, col);
        vertex(b, nb, col);
        vertex(c, nc, col);
    }

    void vertex(P p, const float* n, Colour c) {
        float x = p.x, y = p.y, z = p.z, nx = n[0], ny = n[1], nz = n[2];
        if (front_) {
            std::swap(y, z), std::swap(ny, nz);
            y = -y, ny = -ny;
        }
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 0) nx /= l, ny /= l, nz /= l;
        out_.insert(out_.end(), {x + ox_, y + oy_, z + oz_, nx, ny, nz, 0, 0, 0, c.r, c.g, c.b});
    }

    std::vector<float>& out_;
    float ox_ = 0, oy_ = 0, oz_ = 0;
    bool front_ = false;
};

// The slot of a cartridge: the mouth and its two spring flaps, closed in the middle.
void slot(Builder& b, float x, float z, float y, float w, float d) {
    b.top(x, z, y, w + 0.04f, d + 0.04f, 0.03f, 0.004f, kBlackSoft);
    b.top(x, z, y + 0.004f, w, d, 0.02f, 0.002f, kHole);
    b.top(x, z - d * 0.26f, y + 0.006f, w * 0.97f, d * 0.44f, 0.006f, 0.002f, kFlap);
    b.top(x, z + d * 0.26f, y + 0.006f, w * 0.97f, d * 0.44f, 0.006f, 0.002f, kFlap);
}

constexpr Colour kBoard{0.06f, 0.24f, 0.12f};
constexpr Colour kContact{0.8f, 0.62f, 0.24f};

float mm(float v) { return v / 135.0f; }

// A handheld standing up: its body, the screen's frame (the screen itself is drawn apart,
// to light up), the pad, A and B, Select and Start, the speaker and the slot on top.
void handheld(Builder& b, float w, float h, float frontZ, float depth, const float* slotAt, const float* screen,
              const float* led, Colour body, Colour bezel) {
    b.frame(0, h / 2, 0, true);
    b.loft(0, 0, w, h, 0.06f, {{frontZ - depth, 0.01f}, {frontZ - depth + 0.012f, 0}, {frontZ - 0.012f, 0}, {frontZ, 0.01f}}, body, true, 8);
    b.frame(0, 0, 0, false);
    const float z = frontZ;
    b.front(screen[0], screen[1], z, screen[2] + 0.2f, screen[3] + 0.13f, 0.03f, 0.002f, bezel);
    const float padY = screen[1] - screen[3] / 2 - 0.3f;
    b.front(-w * 0.27f, padY, z, 0.2f, 0.065f, 0.012f, 0.02f, kBlackSoft);
    b.front(-w * 0.27f, padY, z, 0.065f, 0.2f, 0.012f, 0.02f, kBlackSoft);
    b.front(w * 0.33f, padY + 0.04f, z, 0.11f, 0.11f, 0.055f, 0.018f, kGbMagenta);
    b.front(w * 0.14f, padY - 0.02f, z, 0.11f, 0.11f, 0.055f, 0.018f, kGbMagenta);
    b.front(-0.07f, padY - 0.2f, z, 0.1f, 0.03f, 0.015f, 0.008f, kButton);
    b.front(0.07f, padY - 0.2f, z, 0.1f, 0.03f, 0.015f, 0.008f, kButton);
    for (int i = 0; i < 5; ++i) b.front(w * 0.28f + 0.04f * static_cast<float>(i), 0.1f, z, 0.015f, 0.14f, 0.006f, 0.002f, kHole);
    b.front(led[0], led[1], z, 0.035f, 0.035f, 0.017f, 0.002f, kBlackSoft);
    slot(b, slotAt[0], slotAt[2], slotAt[1] - 0.006f, 0.45f, 0.09f);
}

void gameBoy(Builder& b) {
    using L = ConsoleLayout;
    handheld(b, mm(90), L::kGbSlot[1], L::kGbFrontZ, mm(32), L::kGbSlot, L::kGbScreen, L::kGbLed, kGbGrey, kGbBezel);
}

void gameBoyColor(Builder& b) {
    using L = ConsoleLayout;
    handheld(b, mm(78), L::kGbcSlot[1], L::kGbcFrontZ, mm(27.4f), L::kGbcSlot, L::kGbcScreen, L::kGbcLed, kGbcTeal, kBlack);
}

void nes(Builder& b) {
    using L = ConsoleLayout;
    // The dark lower half, the light upper one; the door at the front, the two buttons
    // and the light at its left, and the ribs on the top.
    const float w = mm(254), d = mm(203), mid = L::kNesHeight * 0.48f, front = L::kNesDoor[2];
    b.loft(0, front - d / 2, w, d, 0.01f, {{0, 0}, {mid, 0}}, kNesDark, true, 3);
    b.loft(0, front - d / 2, w, d, 0.01f, {{mid, 0}, {L::kNesHeight - 0.01f, 0}, {L::kNesHeight, 0.008f}}, kNesLight, false, 3);
    b.front(L::kNesDoor[0], L::kNesDoor[1], front, mm(140), mm(32), 0.01f, 0.003f, kNesLight);
    b.front(L::kNesDoor[0], L::kNesDoor[1] - mm(16), front, mm(140), mm(2), 0.002f, 0.004f, kGroove);
    b.front(L::kNesLed[0] + 0.14f, L::kNesLed[1], front, 0.12f, 0.08f, 0.01f, 0.012f, kButton);
    b.front(L::kNesLed[0] + 0.3f, L::kNesLed[1], front, 0.12f, 0.08f, 0.01f, 0.012f, kButton);
    for (int i = 0; i < 9; ++i) b.top(0.35f, front - 0.25f - 0.06f * static_cast<float>(i), L::kNesHeight, 0.9f, 0.02f, 0.005f, 0.002f, kGroove);
}

void snes(Builder& b) {
    using L = ConsoleLayout;
    // The body with its sloped front, the darker deck, the slot, POWER, RESET and EJECT.
    const float w = mm(200), d = mm(242);
    b.loft(0, 0, w, d, 0.06f, {{0, 0.02f}, {0.02f, 0}, {L::kSnesHeight - 0.08f, 0}, {L::kSnesHeight - 0.02f, 0.03f}}, kSnesGrey, true, 6);
    b.top(0, -0.05f, L::kSnesHeight - 0.02f, w * 0.85f, d * 0.75f, 0.05f, 0.008f, kSnesDeck);
    slot(b, L::kSnesSlot[0], L::kSnesSlot[2], L::kSnesSlot[1] - 0.006f, mm(138), mm(26));
    b.top(-0.52f, 0.3f, L::kSnesHeight - 0.012f, 0.2f, 0.25f, 0.03f, 0.02f, kSnesGrey);
    b.top(0.52f, 0.3f, L::kSnesHeight - 0.012f, 0.2f, 0.25f, 0.03f, 0.02f, kSnesGrey);
    b.top(0, 0.3f, L::kSnesHeight - 0.012f, 0.3f, 0.14f, 0.05f, 0.01f, kSnesPurple);
}


// The board's edge at the bottom of a cartridge, inside the shell's opening, with its
// row of gold contacts on both faces (`pitch` mm apart).
void contacts(Builder& b, float halfW, float bottom, int count, float pitch, float board) {
    b.slab(mm(-halfW - 1.5f), bottom - mm(0.2f), mm(halfW + 1.5f), bottom + mm(2.5f), mm(-board / 2 - 2.2f), mm(board / 2 + 2.2f), kHole);
    b.slab(mm(-halfW), bottom + mm(0.4f), mm(halfW), bottom + mm(6), mm(-board / 2), mm(board / 2), kBoard);
    const float first = -pitch * static_cast<float>(count - 1) / 2;
    for (int i = 0; i < count; ++i) {
        const float x = first + pitch * static_cast<float>(i);
        for (float side : {1.0f, -1.0f})
            b.slab(mm(x - pitch * 0.32f), bottom + mm(0.4f), mm(x + pitch * 0.32f), bottom + mm(5.2f),
                   side > 0 ? mm(board / 2) : mm(-board / 2 - 0.05f), side > 0 ? mm(board / 2 + 0.05f) : mm(-board / 2), kContact);
    }
}

// Ribs across the shell to hold it by, on its front (`side` 1) or back (-1).
void ribs(Builder& b, float halfW, float y0, float y1, int count, float t, float side) {
    for (int i = 0; i < count; ++i) {
        const float y = y0 + (y1 - y0) * static_cast<float>(i) / static_cast<float>(std::max(1, count - 1));
        const float z = side * t / 2;
        b.slab(mm(-halfW), mm(y - 0.6f), mm(halfW), mm(y + 0.6f), std::min(z, z + side * mm(0.7f)), std::max(z, z + side * mm(0.7f)), kGroove);
    }
}

// The groove around the label's recess.
void recess(Builder& b, const CartridgeShape& c) {
    const float z = c.d / 2, e = mm(1.2f);
    const float x0 = -c.labelW / 2 - e, x1 = c.labelW / 2 + e, y0 = c.labelY - c.labelH / 2 - e, y1 = c.labelY + c.labelH / 2 + e;
    b.slab(x0, y0, x1, y1, z, z + mm(0.2f), kGroove);
}

}  // namespace

CartridgeShape cartridgeShape(ConsoleSystem s) {
    switch (s) {
        // Game Boy (and Color): 57 x 65 x 8 mm, the label in the lower part; it goes into
        // the slot all but its top edge.
        case ConsoleSystem::GameBoy:
        case ConsoleSystem::GameBoyColor: return {mm(57), mm(65), mm(8), mm(47), mm(38), mm(-8), mm(61), mm(60)};
        // NES: 120 x 133 x 20 mm, the label over the upper part of the front.
        case ConsoleSystem::Nes: return {mm(120), mm(133), mm(20), mm(100), mm(76), mm(20), mm(100), mm(125)};
        // Super Nintendo (the European and Japanese mould): 134 x 88 x 20 mm, about half of it
        // in the slot.
        default: return {mm(134), mm(88), mm(20), mm(104), mm(52), mm(8), mm(48), mm(138)};
    }
}

void buildCartridgeModel(ConsoleSystem s, std::vector<float>& out) {
    Builder b(out);
    const CartridgeShape c = cartridgeShape(s);
    const float t = c.d / 2;
    switch (s) {
        case ConsoleSystem::GameBoy:
        case ConsoleSystem::GameBoyColor:
            // Its top right corner cut off; the ridges to hold it by above the label.
            b.prismZ(-t, t, {{mm(-28.5f), mm(-32.5f)}, {mm(28.5f), mm(-32.5f)}, {mm(28.5f), mm(28.5f)}, {mm(24.5f), mm(32.5f)},
                             {mm(-27.5f), mm(32.5f)}, {mm(-28.5f), mm(31.5f)}}, s == ConsoleSystem::GameBoyColor ? kBlack : kGbGrey);
            ribs(b, 20, 20, 28, 3, c.d, 1);
            contacts(b, 24, mm(-32.5f), 32, 1.5f, 1.2f);
            break;
        case ConsoleSystem::Nes:
            b.prismZ(-t, t, {{mm(-60), mm(-66.5f)}, {mm(60), mm(-66.5f)}, {mm(60), mm(66.5f)}, {mm(-60), mm(66.5f)}}, kNesDark);
            ribs(b, 50, -60, -30, 10, c.d, 1);
            contacts(b, 45, mm(-66.5f), 36, 2.54f, 1.6f);
            break;
        default:
            b.prismZ(-t, t, {{mm(-62), mm(-44)}, {mm(62), mm(-44)}, {mm(62), mm(30)}, {mm(67), mm(36)}, {mm(62), mm(44)},
                             {mm(-62), mm(44)}, {mm(-67), mm(36)}, {mm(-62), mm(30)}}, kSnesGrey);
            ribs(b, 55, -38, -20, 5, c.d, -1);
            contacts(b, 38, mm(-44), 31, 2.5f, 1.6f);
            break;
    }
    recess(b, c);
}

ConsoleModel consoleModelOf(ConsoleSystem s) {
    switch (s) {
        case ConsoleSystem::GameBoyColor: return ConsoleModel::GameBoyColor;
        case ConsoleSystem::Nes: return ConsoleModel::Nes;
        case ConsoleSystem::Snes: return ConsoleModel::Snes;
        default: return ConsoleModel::GameBoy;
    }
}

void buildConsoleModel(ConsoleModel model, std::vector<float>& out) {
    Builder b(out);
    switch (model) {
        case ConsoleModel::GameBoy: gameBoy(b); break;
        case ConsoleModel::GameBoyColor: gameBoyColor(b); break;
        case ConsoleModel::Nes: nes(b); break;
        case ConsoleModel::Snes: snes(b); break;
        default: break;
    }
}

}  // namespace plumnin
