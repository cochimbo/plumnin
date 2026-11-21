#include "room.h"

#include "gl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace plumnin {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kM = 1.0f / 0.135f;  // a metre in the shelf's units

// ---- Drawing the textures -------------------------------------------------------------

struct Image {
    int w, h;
    std::vector<uint32_t> px;
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width * height)) {}
    void set(int x, int y, float r, float g, float b) {
        auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        px[static_cast<size_t>(y * w + x)] = c(r) | c(g) << 8 | c(b) << 16 | 0xFF000000u;
    }
};

float hash(int x, int y, int seed = 0) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}

// Smooth noise from 0 to 1 that repeats every `period` cells.
float noise(float x, float y, int period, int seed = 0) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto at = [&](int i, int j) { return hash(((i % period) + period) % period, ((j % period) + period) % period, seed); };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

float fbm(float x, float y, int period, int seed = 0) {
    float sum = 0, amp = 0.5f;
    for (int o = 0; o < 4; ++o) {
        sum += noise(x, y, period, seed + o) * amp;
        x *= 2, y *= 2, period *= 2, amp *= 0.5f;
    }
    return sum / 0.9375f;
}

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Cream paper with brown stripes in pairs and a small diamond between them: half a metre
// of it each time it repeats.
Image wallpaper() {
    Image im(256, 256);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(im.h);
            const float grain = 0.94f + 0.06f * fbm(u * 16, v * 16, 16, 3);
            float r = 0.86f, g = 0.78f, b = 0.62f;
            const float s = std::fmod(u, 0.5f);
            if ((s > 0.02f && s < 0.05f) || (s > 0.07f && s < 0.085f)) r = 0.55f, g = 0.36f, b = 0.22f;
            else if (s > 0.05f && s < 0.07f) r = 0.78f, g = 0.62f, b = 0.42f;
            // The diamonds, between the pairs of stripes.
            for (float cy : {0.25f, 0.75f}) {
                const float d = std::fabs(s - 0.29f) + std::fabs(v - cy);
                if (d < 0.055f && d > 0.03f) r = 0.70f, g = 0.47f, b = 0.30f;
                else if (d < 0.015f) r = 0.80f, g = 0.42f, b = 0.25f;
            }
            im.set(x, y, r * grain, g * grain, b * grain);
        }
    return im;
}

// Wood grain along v: streaks that wander, from a few octaves of noise.
float grainAt(float u, float v, int seed) {
    const float w = fbm(u * 3, v * 0.6f, 64, seed) * 6;
    return 0.5f + 0.5f * std::sin((u * 40 + w) * kPi);
}

// Dark boards standing side by side, grooves between them: the dado. Eight boards across.
Image panelling() {
    Image im(256, 256);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(im.h);
            const int board = x / 32;
            const float bu = static_cast<float>(x % 32) / 32.0f;
            const float tone = 0.85f + 0.3f * hash(board, 0, 11);
            const float g = grainAt(u * 8, v, board + 20);
            float k = tone * (0.78f + 0.22f * g);
            if (bu < 0.05f || bu > 0.95f) k *= 0.35f;  // the groove
            im.set(x, y, 0.32f * k, 0.19f * k, 0.10f * k);
        }
    return im;
}

// Oak blocks of four slats each, turned a quarter between neighbours: 1.2 m each time it
// repeats, four blocks across.
Image parquet() {
    Image im(512, 512);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const int bx = x / 128, by = y / 128, lx = x % 128, ly = y % 128;
            const bool across = (bx + by) % 2 == 0;
            const int slat = (across ? ly : lx) / 32;
            const int along = across ? lx : ly, side = (across ? ly : lx) % 32;
            const float tone = 0.82f + 0.3f * hash(bx * 4 + slat, by, 7);
            const float g = grainAt(static_cast<float>(side) / 32.0f + static_cast<float>(slat), static_cast<float>(along) / 128.0f,
                                    bx * 31 + by * 7 + slat);
            float k = tone * (0.8f + 0.2f * g);
            if (side == 0 || along == 0) k *= 0.45f;  // the joints
            im.set(x, y, 0.70f * k, 0.48f * k, 0.27f * k);
        }
    return im;
}

// A rug of its time: a burgundy border, a navy band with an orange zigzag, and diamonds
// over a sand field.
Image rug() {
    Image im(512, 352);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(im.h);
            // Distance to the edge, in the rug's own height units.
            const float ax = std::min(u, 1 - u) * static_cast<float>(im.w) / static_cast<float>(im.h), ay = std::min(v, 1 - v);
            const float e = std::min(ax, ay);
            float r, g, b;
            if (e < 0.06f) r = 0.42f, g = 0.09f, b = 0.12f;
            else if (e < 0.075f) r = 0.88f, g = 0.80f, b = 0.64f;
            else if (e < 0.16f) {
                r = 0.10f, g = 0.14f, b = 0.30f;
                // The zigzag runs along the band.
                const float along = ax < ay ? v * static_cast<float>(im.h) : u * static_cast<float>(im.w);
                const float z = std::fabs(std::fmod(along / 22.0f, 2.0f) - 1.0f);
                const float at = 0.085f + z * 0.065f;
                if (std::fabs(e - at) < 0.012f) r = 0.90f, g = 0.48f, b = 0.14f;
            } else if (e < 0.175f) r = 0.88f, g = 0.80f, b = 0.64f;
            else {
                r = 0.80f, g = 0.66f, b = 0.46f;
                // Diamonds in a lattice; the middle one larger.
                const float cx = (u - 0.5f) * static_cast<float>(im.w) / static_cast<float>(im.h), cy = v - 0.5f;
                const float big = std::fabs(cx) / 0.9f + std::fabs(cy) / 0.55f;
                if (big < 0.32f) r = 0.62f, g = 0.18f, b = 0.12f;
                else if (big < 0.40f) r = 0.12f, g = 0.42f, b = 0.44f;
                else {
                    const float lx = std::fmod(cx + 10.0f, 0.18f) - 0.09f, ly = std::fmod(cy + 10.0f, 0.18f) - 0.09f;
                    const float d = std::fabs(lx) + std::fabs(ly);
                    if (d < 0.035f) r = 0.12f, g = 0.42f, b = 0.44f;
                    else if (d < 0.05f) r = 0.70f, g = 0.42f, b = 0.24f;
                }
            }
            const float wool = 0.88f + 0.12f * hash(x, y, 5) * 0.6f + 0.07f * noise(u * 40, v * 40, 40, 9);
            im.set(x, y, r * wool, g * wool, b * wool);
        }
    return im;
}

// A poster of a sunset of the time: a striped sun going down behind purple hills over a
// glowing grid.
Image poster() {
    Image im(256, 352);
    const float horizon = 0.64f;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(im.h);
            float r, g, b;
            if (v < horizon) {
                const float t = v / horizon;
                r = 0.16f + 0.84f * t, g = 0.04f + 0.36f * t * t, b = 0.30f + 0.10f * t;
                // Stars high up.
                if (t < 0.5f && hash(x, y, 2) > 0.995f) r = g = b = 0.9f;
                // The sun, cut by bands that thicken towards the horizon.
                const float dx = (u - 0.5f) * static_cast<float>(im.w) / static_cast<float>(im.h), dy = v - 0.45f;
                if (dx * dx + dy * dy < 0.17f * 0.17f) {
                    const float s = (v - 0.36f) / 0.26f;
                    const bool cut = s > 0 && std::fmod(v * 60.0f, 2.0f) < s * 1.4f;
                    if (!cut) {
                        const float k = (v - 0.28f) / 0.34f;
                        r = 1.0f, g = 0.86f - 0.55f * k, b = 0.30f + 0.25f * k;
                    }
                }
                // The hills.
                const float hill = horizon - 0.05f - 0.06f * std::fabs(std::sin(u * 7.0f + 0.4f)) * (0.6f + 0.4f * std::sin(u * 19.0f));
                if (v > hill) r = 0.20f, g = 0.05f, b = 0.30f;
            } else {
                r = 0.05f, g = 0.01f, b = 0.10f;
                // The grid: lines across, closer together towards the horizon, and lines
                // that meet there.
                const float depth = (v - horizon) / (1 - horizon);
                const float z = 1.0f / (depth + 0.05f);
                const float across = std::fabs(std::fmod(z * 1.2f, 1.0f) - 0.5f);
                const float run = (u - 0.5f) / (depth + 0.05f);
                const float along = std::fabs(std::fmod(run * 1.6f + 100.5f, 1.0f) - 0.5f);
                const float line = std::max(smoothstep(0.47f, 0.5f, across), smoothstep(0.44f, 0.5f, along));
                r += 0.95f * line, g += 0.15f * line, b += 0.75f * line;
            }
            im.set(x, y, r, g, b);
        }
    return im;
}

// What the window looks out on: an evening sky over far roofs.
Image windowView() {
    Image im(256, 256);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(im.h);
            float r = 0.52f + 0.45f * v, g = 0.68f + 0.18f * v, b = 0.92f - 0.25f * v;
            const float cloud = smoothstep(0.55f, 0.8f, fbm(u * 4, v * 8, 4, 13)) * (1 - v);
            r += (1 - r) * cloud, g += (1 - g) * cloud * 0.95f, b += (1 - b) * cloud * 0.9f;
            // Roofs and a few lit windows in them.
            const int block = x / 22;
            const float top = 0.72f + 0.18f * hash(block, 0, 17);
            if (v > top) {
                r = 0.22f, g = 0.22f, b = 0.30f;
                if ((x % 22) > 4 && (x % 22) < 17 && (x % 6) < 3 && (y % 9) < 4 && hash(x / 6, y / 9, 19) > 0.6f)
                    r = 0.95f, g = 0.80f, b = 0.45f;
            }
            im.set(x, y, r, g, b);
        }
    return im;
}

// Mustard cloth in soft folds.
Image curtain() {
    Image im(64, 64);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(im.w);
            const float fold = 0.8f + 0.2f * std::sin(u * 2 * kPi);
            const float weave = 0.95f + 0.05f * hash(x, y, 23);
            const float k = fold * weave;
            im.set(x, y, 0.66f * k, 0.42f * k, 0.17f * k);
        }
    return im;
}

// The front of a video recorder: the cassette's flap, a green clock blinking 12:00 and
// a row of buttons, under a silver strip.
Image vcrFront() {
    Image im(256, 64);
    // The clock's digits, as seven segments: a b c d e f g.
    const int digits[4] = {0x06, 0x5B, 0x3F, 0x3F};  // 1 2 0 0
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            float r = 0.08f, g = 0.08f, b = 0.09f;
            if (y < 14) r = g = b = 0.62f + 0.06f * static_cast<float>(y % 3 == 0);
            // The flap.
            if (x > 16 && x < 120 && y > 24 && y < 40) r = g = b = 0.03f;
            if (x > 18 && x < 118 && y == 26) r = g = b = 0.18f;
            // The display.
            if (x > 140 && x < 216 && y > 22 && y < 46) {
                r = 0.02f, g = 0.06f, b = 0.04f;
                const int lx = x - 146, ly = y - 26;
                const int d = lx / 17, dx = lx % 17;
                if (d >= 0 && d < 4 && ly >= 0 && ly < 16) {
                    const int s = digits[d];
                    auto on = [&](int bit) { return (s >> bit) & 1; };
                    bool lit = false;
                    if (ly < 2 && dx > 1 && dx < 10) lit = on(0);                        // a
                    if (dx >= 9 && dx < 11 && ly > 0 && ly < 8) lit = lit || on(1);      // b
                    if (dx >= 9 && dx < 11 && ly > 8 && ly < 16) lit = lit || on(2);     // c
                    if (ly >= 14 && dx > 1 && dx < 10) lit = lit || on(3);               // d
                    if (dx < 2 && ly > 8 && ly < 16) lit = lit || on(4);                 // e
                    if (dx < 2 && ly > 0 && ly < 8) lit = lit || on(5);                  // f
                    if (ly >= 7 && ly < 9 && dx > 1 && dx < 10) lit = lit || on(6);      // g
                    if (lit) r = 0.35f, g = 1.0f, b = 0.55f;
                }
                if ((x == 179 || x == 180) && (y == 30 || y == 37)) r = 0.35f, g = 1.0f, b = 0.55f;  // the colon
            }
            // The buttons.
            if (y > 50 && y < 57 && x > 140 && x < 250 && (x - 140) % 22 < 16) r = g = b = 0.45f;
            if (y > 50 && y < 57 && x > 16 && x < 40) r = 0.65f, g = 0.12f, b = 0.10f;  // record
            im.set(x, y, r, g, b);
        }
    return im;
}

// ---- Placing the pieces ---------------------------------------------------------------

std::array<float, 16> boxAt(float cx, float cy, float cz, float sx, float sy, float sz) {
    return {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, cx, cy, cz, 1};
}

// A cylinder standing upright, its base's middle at (cx, cy, cz).
std::array<float, 16> standingCylinder(float cx, float cy, float cz, float diameter, float height) {
    return {diameter, 0, 0, 0, 0, 0, diameter, 0, 0, -height, 0, 0, cx, cy + height / 2, cz, 1};
}

}  // namespace

RoomTextures::~RoomTextures() {
    for (unsigned int& id : ids_)
        if (id) gl::DeleteTextures(1, &id);
}

unsigned int RoomTextures::get(int which) {
    if (which < 0 || which >= kRoomTextures) return 0;
    unsigned int& id = ids_[static_cast<size_t>(which)];
    if (id) return id;
    Image im = which == kWallpaper   ? wallpaper()
               : which == kPanelling ? panelling()
               : which == kParquet   ? parquet()
               : which == kRug       ? rug()
               : which == kPoster    ? poster()
               : which == kWindowView ? windowView()
               : which == kCurtain   ? curtain()
                                     : vcrFront();
    gl::GenTextures(1, &id);
    gl::BindTexture(gl::TEXTURE_2D, id);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, im.w, im.h, 0, gl::RGBA, gl::UNSIGNED_BYTE, im.px.data());
    gl::GenerateMipmap(gl::TEXTURE_2D);
    return id;
}

void buildRoom(float width, float height, float floorY, float backZ, std::vector<RoomPiece>& out) {
    // A couple of metres of wall on each side of the shelf, and nearly one above it; the
    // side walls and the ceiling run on past wherever the camera can be.
    const float half = std::max(width / 2 + 2.1f * kM, 2.6f * kM);
    const float tall = std::max(height + 0.9f * kM, 2.6f * kM);
    const float front = 60.0f, thick = 0.2f;
    const float sideLength = front - backZ, sideZ = (front + backZ) / 2;
    const float paper = 0.5f * kM, dado = 0.9f * kM;

    auto add = [&](const std::array<float, 16>& model, std::array<float, 3> colour, int texture = -1, int faces = 0,
                   std::array<float, 4> rect = {0, 0, 1, 1}) -> RoomPiece& {
        RoomPiece p;
        p.model = model;
        p.colour = colour;
        p.texture = texture;
        p.texFaces = texture >= 0 ? faces : 0;
        p.texRect = rect;
        out.push_back(p);
        return out.back();
    };
    const std::array<float, 3> white{1, 1, 1};
    const std::array<float, 3> skirting{0.24f, 0.14f, 0.07f}, cream{0.90f, 0.87f, 0.80f}, brass{0.80f, 0.62f, 0.28f};

    // The walls: papered, a wooden dado along their foot with a rail on it and a skirting
    // board under it.
    add(boxAt(0, floorY + tall / 2, backZ - thick / 2, 2 * half + 2 * thick, tall, thick), white, kWallpaper, 1 << 4,
        {0, 0, 2 * half / paper, tall / paper});
    add(boxAt(-half - thick / 2, floorY + tall / 2, sideZ, thick, tall, sideLength), white, kWallpaper, 1 << 0,
        {0, 0, sideLength / paper, tall / paper});
    add(boxAt(half + thick / 2, floorY + tall / 2, sideZ, thick, tall, sideLength), white, kWallpaper, 1 << 1,
        {0, 0, sideLength / paper, tall / paper});
    const float panel = 0.025f * kM, rail = 0.05f * kM, skirt = 0.1f * kM;
    add(boxAt(0, floorY + dado / 2, backZ + panel / 2, 2 * half, dado, panel), white, kPanelling, 1 << 4,
        {0, 0, 2 * half / (0.6f * kM), 1});
    add(boxAt(-half + panel / 2, floorY + dado / 2, sideZ, panel, dado, sideLength), white, kPanelling, 1 << 0,
        {0, 0, sideLength / (0.6f * kM), 1});
    add(boxAt(half - panel / 2, floorY + dado / 2, sideZ, panel, dado, sideLength), white, kPanelling, 1 << 1,
        {0, 0, sideLength / (0.6f * kM), 1});
    add(boxAt(0, floorY + dado, backZ + panel, 2 * half, rail, 2 * panel), skirting).shine = 0.3f;
    add(boxAt(-half + panel, floorY + dado, sideZ, 2 * panel, rail, sideLength), skirting).shine = 0.3f;
    add(boxAt(half - panel, floorY + dado, sideZ, 2 * panel, rail, sideLength), skirting).shine = 0.3f;
    add(boxAt(0, floorY + skirt / 2, backZ + panel * 1.5f, 2 * half, skirt, panel), skirting);
    add(boxAt(-half + panel * 1.5f, floorY + skirt / 2, sideZ, panel, skirt, sideLength), skirting);
    add(boxAt(half - panel * 1.5f, floorY + skirt / 2, sideZ, panel, skirt, sideLength), skirting);
    // The ceiling, and the moulding where it meets the walls.
    add(boxAt(0, floorY + tall + thick / 2, sideZ, 2 * half + 2 * thick, thick, sideLength), cream).reflect = false;
    const float cornice = 0.08f * kM;
    add(boxAt(0, floorY + tall - cornice / 2, backZ + cornice / 2, 2 * half, cornice, cornice), cream).reflect = false;
    add(boxAt(-half + cornice / 2, floorY + tall - cornice / 2, sideZ, cornice, cornice, sideLength), cream).reflect = false;
    add(boxAt(half - cornice / 2, floorY + tall - cornice / 2, sideZ, cornice, cornice, sideLength), cream).reflect = false;

    // The rug in front of the shelf, where the console comes up.
    {
        const float w = 2.6f * kM, d = 1.8f * kM;
        RoomPiece& r = add(boxAt(0, floorY + 0.004f, backZ + 0.75f + d / 2, w, 0.008f, d), white, kRug, 1 << 2);
        r.reflect = false;
        r.shine = 0;
    }

    // To the left of the shelf: a window with its curtains, and a floor lamp in the corner.
    const float left = -width / 2;
    {
        const float wx = std::max(left - 1.15f * kM, -half + 0.95f * kM), ww = 1.0f * kM, wh = 1.15f * kM, sill = floorY + 0.95f * kM;
        const float wy = sill + wh / 2, z = backZ + 0.01f;
        RoomPiece& view = add(boxAt(wx, wy, z, ww, wh, 0.02f), white, kWindowView, 1 << 4);
        view.glow = 0.55f;
        view.reflect = false;
        const float bar = 0.06f * kM, depth = 0.08f * kM;
        add(boxAt(wx, sill + wh - bar / 2, z + depth / 2, ww + bar, bar, depth), cream);
        add(boxAt(wx - ww / 2, wy, z + depth / 2, bar, wh, depth), cream);
        add(boxAt(wx + ww / 2, wy, z + depth / 2, bar, wh, depth), cream);
        add(boxAt(wx, wy, z + depth / 2, bar * 0.6f, wh, depth * 0.8f), cream);
        add(boxAt(wx, wy + wh * 0.12f, z + depth / 2, ww, bar * 0.6f, depth * 0.8f), cream);
        add(boxAt(wx, sill - bar / 2, z + depth, ww + 0.2f * kM, bar, 2 * depth), cream).shine = 0.3f;
        // The curtains, gathered at the sides, on a brass rod.
        const float top = sill + wh + 0.2f * kM, cw = 0.38f * kM, cd = 0.06f * kM, drop = top - (sill - 0.25f * kM);
        for (int side : {-1, 1}) {
            const float cx = wx + static_cast<float>(side) * (ww / 2 + cw / 2 - 0.08f * kM);
            add(boxAt(cx, top - drop / 2, z + 0.15f * kM, cw, drop, cd), white, kCurtain, 1 << 4, {0, 0, 4, 1});
        }
        RoomPiece& rod = add(boxAt(wx, top + 0.02f * kM, z + 0.15f * kM, ww + 2 * cw + 0.15f * kM, 0.03f * kM, 0.03f * kM), brass);
        rod.shine = 0.8f;

        // The lamp: a heavy round foot, a brass stem and a cream shade lit from inside.
        const float lx = std::max(-half + 0.4f * kM, std::min(wx - ww / 2 - 0.55f * kM, left - 0.4f * kM)), lz = backZ + 0.45f * kM;
        RoomPiece& foot = add(standingCylinder(lx, floorY, lz, 0.32f * kM, 0.03f * kM), {0.15f, 0.12f, 0.1f});
        foot.cylinder = true;
        foot.shine = 0.5f;
        RoomPiece& stem = add(standingCylinder(lx, floorY, lz, 0.025f * kM, 1.45f * kM), brass);
        stem.cylinder = true;
        stem.shine = 0.9f;
        RoomPiece& shade = add(standingCylinder(lx, floorY + 1.35f * kM, lz, 0.42f * kM, 0.32f * kM), {0.92f, 0.76f, 0.50f});
        shade.cylinder = true;
        shade.glow = 0.3f;
        shade.shine = 0;
    }

    // To the right: the television on a walnut cabinet with the video recorder on its
    // shelf, and the poster over them.
    {
        const float cw = 1.1f * kM, ch = 0.52f * kM, cd = 0.45f * kM, board = 0.025f * kM;
        const float cx = std::min(width / 2 + 0.35f * kM + cw / 2, half - cw / 2 - 0.15f * kM), cz = backZ + 0.02f + cd / 2;
        const std::array<float, 3> walnut{0.30f, 0.17f, 0.09f}, inside{0.16f, 0.09f, 0.05f};
        // Walnut: a strip of the dado's boards, between their grooves; the inside plain.
        auto wood = [&](float x, float y, float z, float sx, float sy, float sz, std::array<float, 3> c) {
            if (c == inside) add(boxAt(x, y, z, sx, sy, sz), c);
            else add(boxAt(x, y, z, sx, sy, sz), white, kPanelling, 0x3F, {0.04f, 0, 0.11f, 1}).shine = 0.35f;
        };
        const float feet = 0.06f * kM, y0 = floorY + feet;
        for (int fx : {-1, 1})
            for (int fz : {-1, 1})
                add(boxAt(cx + static_cast<float>(fx) * (cw / 2 - 0.05f * kM), floorY + feet / 2, cz + static_cast<float>(fz) * (cd / 2 - 0.05f * kM),
                          0.04f * kM, feet, 0.04f * kM), {0.1f, 0.08f, 0.06f});
        wood(cx, y0 + board / 2, cz, cw, board, cd, walnut);
        wood(cx, y0 + ch - board / 2, cz, cw, board, cd, walnut);
        wood(cx - cw / 2 + board / 2, y0 + ch / 2, cz, board, ch, cd, walnut);
        wood(cx + cw / 2 - board / 2, y0 + ch / 2, cz, board, ch, cd, walnut);
        wood(cx, y0 + ch / 2, cz - cd / 2 + board / 2, cw, ch, board, inside);
        wood(cx, y0 + ch * 0.45f, cz, cw - 2 * board, board, cd - board, walnut);
        // Smoked glass doors over the lower half, with brass knobs.
        RoomPiece& glass = add(boxAt(cx, y0 + ch * 0.23f, cz + cd / 2 - board / 2, cw - 2 * board, ch * 0.4f, board / 2), {0.05f, 0.04f, 0.04f});
        glass.shine = 1.2f;
        add(boxAt(cx, y0 + ch * 0.23f, cz + cd / 2 - board / 2, 0.01f * kM, ch * 0.4f, board / 2 + 0.002f), walnut);
        for (int s : {-1, 1}) {
            RoomPiece& knob = add(boxAt(cx + static_cast<float>(s) * 0.04f * kM, y0 + ch * 0.3f, cz + cd / 2, 0.02f * kM, 0.02f * kM, 0.03f * kM), brass);
            knob.cylinder = true;
            knob.shine = 0.9f;
        }
        // The video recorder, on the shelf.
        const float vw = 0.43f * kM, vh = 0.09f * kM, vd = 0.32f * kM, vy = y0 + ch * 0.45f + board / 2 + vh / 2;
        RoomPiece& vcr = add(boxAt(cx - 0.12f * kM, vy, cz + cd / 2 - vd / 2 - 0.03f * kM, vw, vh, vd), {0.1f, 0.1f, 0.11f}, kVcrFront, 1 << 4);
        vcr.colour = {0.55f, 0.55f, 0.57f};
        vcr.glow = 0.12f;
        vcr.shine = 0.5f;
        // A couple of tapes lying beside it.
        add(boxAt(cx + 0.28f * kM, y0 + ch * 0.45f + board / 2 + 0.0125f * kM, cz + 0.05f * kM, 0.19f * kM, 0.025f * kM, 0.105f * kM), {0.06f, 0.06f, 0.07f});
        add(boxAt(cx + 0.29f * kM, y0 + ch * 0.45f + board / 2 + 0.0375f * kM, cz + 0.04f * kM, 0.19f * kM, 0.025f * kM, 0.105f * kM), {0.75f, 0.15f, 0.12f});

        // The television on top.
        RoomPiece tv;
        tv.model = boxAt(cx, y0 + ch, cz + 0.02f * kM, 1, 1, 1);
        tv.television = true;
        out.push_back(tv);

        // The poster over it, in a thin black frame.
        const float pw = 0.5f * kM, ph = pw * 352.0f / 256.0f, py = std::max(y0 + ch + 0.5f * kM + ph / 2 + 0.15f * kM, floorY + 1.6f * kM);
        if (py + ph / 2 < floorY + tall - 0.15f * kM) {
            add(boxAt(cx, py, backZ + 0.015f * kM, pw + 0.04f * kM, ph + 0.04f * kM, 0.02f * kM), {0.05f, 0.05f, 0.05f}).shine = 0.6f;
            add(boxAt(cx, py, backZ + 0.026f * kM, pw, ph, 0.004f * kM), white, kPoster, 1 << 4).shine = 0.5f;
        }
    }
}

}  // namespace plumnin
