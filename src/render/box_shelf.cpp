#include "box_shelf.h"

#include "console_models.h"

#include <algorithm>
#include <cmath>

namespace plumnin {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRad = kPi / 180.0f;

// The shelf's measures are plumsg's, made for a Mega Drive case: 13.5 x 19 x 2.5 cm, with
// 13.5 cm as 1. The boxes of each system have their own size (boxSize).
constexpr float kBoxW = 1.0f, kBoxH = 1.4f, kBoxD = 0.2f, kWall = 0.02f;
constexpr float kFloorY = -kBoxH / 2;   // the boxes stand on the floor
constexpr float kConsoleZ = 1.6f;       // the console sits on the floor in front of them
constexpr float kLidOpen = 165.0f;      // degrees, all the way open
constexpr float kFov = 34.0f;           // degrees, top to bottom
// The shelf behind: the boxes' middle, their spacing, and its boards.
constexpr float kShelfZ = -1.6f, kSpacing = kBoxD + 0.012f;
constexpr float kBoard = 0.045f, kPlinth = 0.09f, kSide = 0.05f, kRowH = kBoxH + 0.1f + kBoard;
const float kBackground[3] = {0.035f, 0.037f, 0.045f};

// Each vertex: position, normal, texture coordinates, the face it belongs to and its colour.
constexpr int kFloats = kConsoleVertexFloats;

constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in float aFace;
layout(location = 4) in vec3 aColour;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform int uMirror;
uniform float uFloorY;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUv;
out vec3 vColour;
flat out int vFace;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vec3 n = transpose(inverse(mat3(uModel))) * aNormal;
    if (uMirror == 1) {
        w.y = 2.0 * uFloorY - w.y;
        n.y = -n.y;
    }
    vWorld = w.xyz;
    vNormal = n;
    vUv = aUv;
    vColour = aColour;
    vFace = int(aFace + 0.5);
    gl_Position = uViewProj * w;
}
)";

// A key light from the top left, a weak fill from the right, a glossy highlight and a rim.
// The floor fades into the background away from the boxes; what it reflects fades
// with the distance below it.
constexpr const char* kFragment = R"(#version 330 core
uniform sampler2D uImage;
uniform sampler2D uNormals;
uniform int uHasNormals;
uniform sampler2D uLabel;
uniform int uHasLabel;
uniform vec4 uLabelArea;
uniform vec4 uLabelRect;
uniform int uLabelTurn;  // quarter turns of the label in the model's texture
uniform vec3 uColour;
uniform int uTexFaces;
uniform vec4 uTexRect;
uniform int uMask;        // the texture's see-through parts are holes
uniform float uGlow;
uniform float uShine;
uniform vec3 uEye;
uniform int uMirror;
uniform int uFloor;
uniform float uFloorY;
uniform vec3 uBackground;
uniform vec4 uFloorFade;  // where the floor is brightest (x, z) and how far it fades out
uniform int uMatte;       // the room around the shelf
uniform float uFloorTile; // how far the floor's texture repeats
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUv;
in vec3 vColour;
flat in int vFace;
out vec4 outColor;
void main() {
    if (uFloor == 1) {
        float r = length(vWorld.xz - uFloorFade.xy);
        if (uTexFaces != 0) {
            // The room's parquet, in the lamp light around what is looked at.
            vec3 wood = texture(uImage, vWorld.xz / uFloorTile).rgb;
            outColor = vec4(mix(wood * 0.95, wood * 0.5, smoothstep(uFloorFade.z, uFloorFade.w * 1.6, r)), 0.86);
            return;
        }
        outColor = vec4(mix(uColour, uBackground, smoothstep(uFloorFade.z, uFloorFade.w, r)), 0.84);
        return;
    }
    // What is still under the floor (the console rising) reflects nothing.
    if (uMirror == 1 && vWorld.y > uFloorY) discard;
    bool textured = ((uTexFaces >> vFace) & 1) != 0;
    vec4 image = textured ? texture(uImage, mix(uTexRect.xy, uTexRect.zw, vUv)) : vec4(1.0);
    if (uMask == 1 && image.a < 0.5) discard;
    vec3 base = textured ? image.rgb * vColour : uColour * vColour;
    // A game's label printed over a cartridge model's own.
    if (uHasLabel == 1 && all(greaterThanEqual(vUv, uLabelArea.xy)) && all(lessThanEqual(vUv, uLabelArea.zw))) {
        vec2 t = (vUv - uLabelArea.xy) / (uLabelArea.zw - uLabelArea.xy);
        for (int i = 0; i < uLabelTurn; ++i) t = vec2(t.y, 1.0 - t.x);
        base = texture(uLabel, mix(uLabelRect.xy, uLabelRect.zw, vec2(t.x, 1.0 - t.y))).rgb;
    }
    vec3 n = normalize(vNormal);
    // A model's normal map, in a frame made from how its texture runs across the screen.
    if (uHasNormals == 1) {
        vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
        vec2 duv1 = dFdx(vUv), duv2 = dFdy(vUv);
        vec3 a = cross(dp2, n), b = cross(n, dp1);
        vec3 t = a * duv1.x + b * duv2.x, bt = a * duv1.y + b * duv2.y;
        float k = inversesqrt(max(max(dot(t, t), dot(bt, bt)), 1e-20));
        vec3 m = texture(uNormals, vUv).xyz * 2.0 - 1.0;
        n = normalize(mat3(t * k, -bt * k, n) * m);
    }
    vec3 l = normalize(vec3(-0.35, 0.8, 0.6));
    vec3 v = normalize(uEye - vWorld);
    float diffuse = max(dot(n, l), 0.0);
    float fill = max(dot(n, normalize(vec3(0.7, 0.1, 0.5))), 0.0) * 0.25;
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 48.0) * uShine;
    float ambient = textured ? 0.45 : 0.34;
    // A cool light from behind outlines the black plastics against the dark.
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * max(dot(n, normalize(vec3(0.2, 0.5, -0.8))) + 0.6, 0.0);
    if (uMatte == 1) rim = 0.0;
    vec3 c = base * (ambient + 0.75 * diffuse + fill) + vec3(spec) + base * uGlow + vec3(0.16, 0.18, 0.24) * rim;
    // The room's light comes from low down: its walls darken towards the ceiling.
    if (uMatte == 1) c *= 1.0 - 0.35 * smoothstep(uFloorY + 9.0, uFloorY + 20.0, vWorld.y);
    if (uMirror == 1) c *= 0.4 * (1.0 - smoothstep(0.0, 1.1, uFloorY - vWorld.y));
    outColor = vec4(c, 1.0);
}
)";

}  // namespace

// Column-major 4x4 matrices, as OpenGL expects them.
namespace {

using Mat4 = std::array<float, 16>;

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

Mat4 translate(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
Mat4 scale(float x, float y, float z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}; }

// Turns +z towards +x for positive degrees.
Mat4 rotateY(float degrees) {
    const float c = std::cos(degrees * kRad), s = std::sin(degrees * kRad);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

// Turns +z towards -y for positive degrees (so -90 lays a face looking at the viewer on its back).
Mat4 rotateX(float degrees) {
    const float c = std::cos(degrees * kRad), s = std::sin(degrees * kRad);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

// Turns +x towards +y for positive degrees.
Mat4 rotateZ(float degrees) {
    const float c = std::cos(degrees * kRad), s = std::sin(degrees * kRad);
    return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// Turns as unit quaternions (w, x, y, z), the same ways as rotateX and rotateY.
using Quat = std::array<float, 4>;
constexpr Quat kNoTurn{1, 0, 0, 0};

Quat operator*(const Quat& a, const Quat& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Quat normalized(const Quat& q) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return n > 0 ? Quat{q[0] / n, q[1] / n, q[2] / n, q[3] / n} : kNoTurn;
}

Quat quatX(float degrees) { return {std::cos(degrees * kRad / 2), std::sin(degrees * kRad / 2), 0, 0}; }
Quat quatY(float degrees) { return {std::cos(degrees * kRad / 2), 0, std::sin(degrees * kRad / 2), 0}; }

// From `a` (t = 0) to `b` (t = 1) the short way round.
Quat towards(const Quat& a, Quat b, float t) {
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) b = {-b[0], -b[1], -b[2], -b[3]};
    return normalized({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
}

Mat4 rotation(const Quat& q) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z),     2 * (x * z - w * y),     0,
            2 * (x * y - w * z),     1 - 2 * (x * x + z * z), 2 * (y * z + w * x),     0,
            2 * (x * z + w * y),     2 * (y * z - w * x),     1 - 2 * (x * x + y * y), 0,
            0,                       0,                       0,                       1};
}

Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY / 2);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1;
    m[14] = 2 * zFar * zNear / (zNear - zFar);
    return m;
}

// The inverse of a matrix that only turns, scales and moves.
Mat4 affineInverse(const Mat4& m) {
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return identity();
    const float k = 1.0f / det;
    // Rows of the inverse of the 3x3 part.
    const float r00 = A * k, r01 = -(b * i - c * h) * k, r02 = (b * f - c * e) * k;
    const float r10 = B * k, r11 = (a * i - c * g) * k, r12 = -(a * f - c * d) * k;
    const float r20 = C * k, r21 = -(a * h - b * g) * k, r22 = (a * e - b * d) * k;
    const float tx = m[12], ty = m[13], tz = m[14];
    return {r00, r10, r20, 0, r01, r11, r21, 0, r02, r12, r22, 0,
            -(r00 * tx + r01 * ty + r02 * tz), -(r10 * tx + r11 * ty + r12 * tz), -(r20 * tx + r21 * ty + r22 * tz), 1};
}

float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

BoxShelf::BoxShelf() {
    program_ = gl::buildProgram(kVertex, kFragment);
    // A unit cube, then a cylinder of radius 0.5 along z, both centred on the origin.
    // Faces: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z (the front), 5 -z; texture coordinates run as
    // the face is seen from outside, with v from its top.
    std::vector<float> v;
    auto vertex = [&v](float px, float py, float pz, float nx, float ny, float nz, float u, float t, int face) {
        v.insert(v.end(), {px, py, pz, nx, ny, nz, u, t, static_cast<float>(face), 1, 1, 1});
    };
    const float faces[6][9] = {{1, 0, 0, 0, 0, -1, 0, 1, 0}, {-1, 0, 0, 0, 0, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 0, 0, 0, -1},
                               {0, -1, 0, 1, 0, 0, 0, 0, 1}, {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, -1, 0, 0, 0, 1, 0}};
    const float corners[6][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int f = 0; f < 6; ++f) {
        const float* n = faces[f];
        for (const auto& c : corners) {
            const float a = c[0], b = c[1];
            vertex(n[0] * 0.5f + n[3] * a + n[6] * b, n[1] * 0.5f + n[4] * a + n[7] * b, n[2] * 0.5f + n[5] * a + n[8] * b,
                   n[0], n[1], n[2], a + 0.5f, 0.5f - b, f);
        }
    }
    cubeFirst_ = 0;
    cubeCount_ = static_cast<int>(v.size()) / kFloats;
    constexpr int kSides = 48;
    for (int s = 0; s < kSides; ++s) {
        const float a0 = 2 * kPi * s / kSides, a1 = 2 * kPi * (s + 1) / kSides;
        const float x0 = 0.5f * std::cos(a0), y0 = 0.5f * std::sin(a0), x1 = 0.5f * std::cos(a1), y1 = 0.5f * std::sin(a1);
        // The caps, mapped like the cube's front and back.
        vertex(0, 0, 0.5f, 0, 0, 1, 0.5f, 0.5f, 4);
        vertex(x0, y0, 0.5f, 0, 0, 1, x0 + 0.5f, 0.5f - y0, 4);
        vertex(x1, y1, 0.5f, 0, 0, 1, x1 + 0.5f, 0.5f - y1, 4);
        vertex(0, 0, -0.5f, 0, 0, -1, 0.5f, 0.5f, 5);
        vertex(x1, y1, -0.5f, 0, 0, -1, 0.5f - x1, 0.5f - y1, 5);
        vertex(x0, y0, -0.5f, 0, 0, -1, 0.5f - x0, 0.5f - y0, 5);
        // The side.
        const float nx0 = std::cos(a0), ny0 = std::sin(a0), nx1 = std::cos(a1), ny1 = std::sin(a1);
        vertex(x0, y0, -0.5f, nx0, ny0, 0, 0, 1, 0);
        vertex(x1, y1, -0.5f, nx1, ny1, 0, 0, 1, 0);
        vertex(x1, y1, 0.5f, nx1, ny1, 0, 0, 0, 0);
        vertex(x0, y0, -0.5f, nx0, ny0, 0, 0, 1, 0);
        vertex(x1, y1, 0.5f, nx1, ny1, 0, 0, 0, 0);
        vertex(x0, y0, 0.5f, nx0, ny0, 0, 0, 0, 0);
    }
    cylinderFirst_ = cubeCount_;
    cylinderCount_ = static_cast<int>(v.size()) / kFloats - cubeCount_;
    // The consoles, then the cartridges' shells.
    for (int m = 0; m < kMeshes; ++m) {
        meshFirst_[m] = static_cast<int>(v.size()) / kFloats;
        if (m < static_cast<int>(ConsoleModel::Count)) buildConsoleModel(static_cast<ConsoleModel>(m), v);
        else
            buildCartridgeModel(static_cast<ConsoleSystem>(m - kCartMeshes), v);
        meshCount_[m] = static_cast<int>(v.size()) / kFloats - meshFirst_[m];
    }

    vertexArray(v, &vao_, &vbo_);
}

void BoxShelf::vertexArray(const std::vector<float>& v, gl::GLuint* vao, gl::GLuint* vbo) {
    gl::GenVertexArrays(1, vao);
    gl::BindVertexArray(*vao);
    gl::GenBuffers(1, vbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, *vbo);
    gl::BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(v.size() * sizeof(float)), v.data(), gl::STATIC_DRAW);
    const int stride = kFloats * sizeof(float);
    const int sizes[5] = {3, 3, 2, 1, 3};
    for (int a = 0, offset = 0; a < 5; offset += sizes[a], ++a) {
        gl::EnableVertexAttribArray(static_cast<gl::GLuint>(a));
        gl::VertexAttribPointer(static_cast<gl::GLuint>(a), sizes[a], gl::FLOAT, gl::FALSE_, stride,
                                reinterpret_cast<const void*>(static_cast<size_t>(offset) * sizeof(float)));
    }
    gl::BindVertexArray(0);
}

void BoxShelf::addRoom() {
    // A few dozen pieces, laid out again each frame for the shelf as it is.
    float w, h, bottom;
    shelfSize(&w, &h, &bottom);
    const float back = kShelfZ - 0.03f - (kBoxW + 0.12f) / 2 - 0.02f;
    room_.clear();
    buildRoom(w, h, bottom, back, room_);
    for (const RoomPiece& r : room_) {
        Part p;
        p.matte = true;
        p.colour = {r.colour[0], r.colour[1], r.colour[2]};
        p.shine = r.shine;
        p.glow = r.glow;
        p.reflect = r.reflect;
        p.cylinder = r.cylinder;
        if (r.television) {
            const Custom& tv = custom_[kRoomCustom];
            if (!tv.vao) continue;
            p.custom = kRoomCustom;
            p.model = r.model * tv.fit;
        } else {
            std::copy(r.model.begin(), r.model.end(), p.model.begin());
        }
        if (r.texture >= 0) {
            p.texture = roomTextures_.get(r.texture);
            p.texFaces = r.texFaces;
            p.texRect = r.texRect;
        }
        parts_.push_back(p);
    }
}

void BoxShelf::upload(Custom& c, const GltfModel& model) {
    if (c.vao) gl::DeleteVertexArrays(1, &c.vao);
    if (c.vbo) gl::DeleteBuffers(1, &c.vbo);
    if (!c.textures.empty()) gl::DeleteTextures(static_cast<gl::GLsizei>(c.textures.size()), c.textures.data());
    c = Custom();
    vertexArray(model.vertices, &c.vao, &c.vbo);
    c.batches = model.batches;
    for (const GltfModel::Image& im : model.images) {
        gl::GLuint t = 0;
        gl::GenTextures(1, &t);
        gl::BindTexture(gl::TEXTURE_2D, t);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, im.width, im.height, 0, gl::RGBA, gl::UNSIGNED_BYTE, im.pixels.data());
        gl::GenerateMipmap(gl::TEXTURE_2D);
        c.textures.push_back(t);
    }
}

void BoxShelf::setConsoleModel(ConsoleModel which, const GltfModel& model, float width, float turn, const float slot[3],
                               const float* led, float ledTilt) {
    Custom& c = custom_[static_cast<int>(which)];
    upload(c, model);
    // Turned, its width measured across; then scaled, centred and stood on the floor.
    const float cx = (model.min[0] + model.max[0]) / 2, cz = (model.min[2] + model.max[2]) / 2;
    const float a = turn * kRad;
    const float across = std::fabs(std::cos(a)) * (model.max[0] - model.min[0]) + std::fabs(std::sin(a)) * (model.max[2] - model.min[2]);
    const float k = across > 0 ? width / across : 1;
    c.fit = scale(k, k, k) * rotateY(turn) * translate(-cx, -model.min[1], -cz);
    std::copy(slot, slot + 3, c.slot);
    if (led) {
        c.hasLed = true;
        std::copy(led, led + 3, c.led);
        c.ledTilt = ledTilt;
    }
}

namespace {

// The models that come with plumnin (assets/models, made by tools/models/prepare.py), or the
// user's own of the same consoles: their real width, how they are turned to face +z, and
// the console's own bounds in the file when it has more (the NES's pad). Where their slots,
// screens and lights are, once stood on the floor, is in ConsoleLayout, the same as the
// built-in models'.
struct ModelFile {
    ConsoleModel model;
    const char* file;
    float width, turn;  // 13.5 cm = 1
    const float* box;
};
// The NES by RadioactiveAG, CC BY 4.0: front to -x, its pad and cable beside it.
const float kNesBox[6] = {1.4406f, 0.0033f, -0.031f, 4.5931f, 1.2683f, 4.0864f};
const ModelFile kModelFiles[] = {
    // The Game Boy by lokeig (CC BY-NC 4.0), the Game Boy Color by Wikiti (CC BY 4.0) and the
    // Super Nintendo by quick_loop (CC BY 4.0), all of them already facing +z.
    {ConsoleModel::GameBoy, "gameboy.glb", 90.0f / 135.0f, 0.0f, nullptr},
    {ConsoleModel::GameBoyColor, "gameboycolor.glb", 78.0f / 135.0f, 0.0f, nullptr},
    {ConsoleModel::Nes, "nes.glb", 254.0f / 135.0f, 90.0f, kNesBox},
    {ConsoleModel::Snes, "snes.glb", 200.0f / 135.0f, 0.0f, nullptr},
};

// Each power light: where it is (ConsoleLayout), and how it is tilted towards the front.
struct Led {
    const float* at;
    float tilt;
};
Led ledOf(ConsoleModel m) {
    using L = ConsoleLayout;
    switch (m) {
        case ConsoleModel::GameBoyColor: return {L::kGbcLed, 90.0f};
        case ConsoleModel::Nes: return {L::kNesLed, 90.0f};
        case ConsoleModel::Snes: return {L::kSnesLed, 25.0f};  // on the slope of its front
        default: return {L::kGbLed, 90.0f};
    }
}

// The cartridges' models: how to turn them to stand with the label to the front, where
// their texture has the label (which a scan replaces) and the picture in it (which a
// cover replaces), the label's quarter turns in the texture and the picture's shape.
struct CartridgeFile {
    ConsoleSystem system;
    const char* file;
    float turnX, turnZ;
    float label[4], picture[4];
    int labelTurn;
    float pictureAspect;  // width / height
    float pictureMiddle;  // how far down the cover the part shown on it is centred
    float turnY = 0;      // first, to straighten a model that comes turned in its file
    int labelImage = -1;  // the texture with the label, when it has more
};
const CartridgeFile kCartridgeFiles[] = {
    // The Game Boy's, from lokeig's model: the one in the slot (standing, its label to the
    // back), with the look of the Tetris one beside it. Its label fills the lower part.
    {ConsoleSystem::GameBoy, "gameboy-cartridge.glb", 0, 0, {0.005f, 0.532f, 0.325f, 0.84f}, {0.005f, 0.532f, 0.325f, 0.84f}, 0,
     1.04f, 0.45f, 180.0f},
    {ConsoleSystem::GameBoyColor, "gameboy-cartridge.glb", 0, 0, {0.005f, 0.532f, 0.325f, 0.84f}, {0.005f, 0.532f, 0.325f, 0.84f},
     0, 1.04f, 0.45f, 180.0f},
    // The NES's, from RadioactiveAG's: lying on its back, its top end to -z. Its label (Punch-
    // Out!!'s) is a texture of its own, the third the model reads (after the shell's normal
    // map and colour), all of it.
    {ConsoleSystem::Nes, "nes-cartridge.glb", 90, 0, {0, 0, 1, 1}, {0, 0, 1, 1}, 0, 0.61f, 0.5f, 0.0f, 2},
};

}  // namespace

void BoxShelf::loadConsoleModels(const std::vector<std::string>& folders) {
    for (const CartridgeFile& f : kCartridgeFiles) {
        for (const std::string& folder : folders) {
            GltfModel model;
            std::string error;
            if (!loadGltf(folder + f.file, model, &error)) continue;
            setCartridgeModel(f.system, model, f.turnX, f.turnZ, f.label, f.picture, f.labelTurn, f.pictureAspect, f.pictureMiddle, f.turnY,
                              f.labelImage);
            break;
        }
    }
    // The room's television, stood on its foot and scaled to its real width.
    for (const std::string& folder : folders) {
        GltfModel model;
        std::string error;
        if (!loadGltf(folder + "television.glb", model, &error)) continue;
        Custom& c = custom_[kRoomCustom];
        upload(c, model);
        const float cx = (model.min[0] + model.max[0]) / 2, cz = (model.min[2] + model.max[2]) / 2;
        const float k = model.max[0] > model.min[0] ? kTelevisionWidth / (model.max[0] - model.min[0]) : 1;
        c.fit = scale(k, k, k) * translate(-cx, -model.min[1], -cz);
        break;
    }
    for (const ModelFile& m : kModelFiles) {
        for (const std::string& folder : folders) {
            GltfModel model;
            std::string error;
            if (!loadGltf(folder + m.file, model, &error)) continue;
            if (m.box) {
                std::copy(m.box, m.box + 3, model.min);
                std::copy(m.box + 3, m.box + 6, model.max);
            }
            const Led l = ledOf(m.model);
            const float none[3] = {0, 0, 0};
            setConsoleModel(m.model, model, m.width, m.turn, none, l.at, l.tilt);
            break;
        }
    }
}

void BoxShelf::setCartridgeModel(ConsoleSystem system, const GltfModel& model, float turnX, float turnZ, const float labelArea[4],
                                 const float pictureArea[4], int labelTurn, float pictureAspect, float pictureMiddle, float turnY,
                                 int labelImage) {
    Custom& c = custom_[kCartCustom + static_cast<int>(system)];
    upload(c, model);
    // Turned to stand, then centred and stretched to the cartridge's real size.
    const Mat4 turn = rotateZ(turnZ) * rotateX(turnX) * rotateY(turnY);
    // Measured on its points once turned, not on the corners of the box around it as it
    // comes: turned by turnY, those corners stand out past the cartridge, which would come
    // out too small and squashed (the Master System's, 85% as wide and 70% as tall).
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t k = 0; k + 3 <= model.vertices.size(); k += 12) {
        const float* p = &model.vertices[k];
        for (int i = 0; i < 3; ++i) {
            const float v = turn[i] * p[0] + turn[4 + i] * p[1] + turn[8 + i] * p[2];
            lo[i] = std::min(lo[i], v);
            hi[i] = std::max(hi[i], v);
        }
    }
    const CartridgeShape shape = cartridgeShape(system);
    float size[3], mid[3];
    for (int i = 0; i < 3; ++i) {
        size[i] = std::max(hi[i] - lo[i], 1e-6f);
        mid[i] = (lo[i] + hi[i]) / 2;
    }
    c.fit = scale(shape.w / size[0], shape.h / size[1], shape.d / size[2]) * translate(-mid[0], -mid[1], -mid[2]) * turn;
    std::copy(labelArea, labelArea + 4, c.labelArea);
    std::copy(pictureArea, pictureArea + 4, c.pictureArea);
    c.labelTurn = labelTurn;
    c.labelImage = labelImage;
    c.pictureAspect = pictureAspect;
    c.pictureMiddle = pictureMiddle;
}

BoxShelf::~BoxShelf() {
    for (Custom& c : custom_) {
        if (c.vao) gl::DeleteVertexArrays(1, &c.vao);
        if (c.vbo) gl::DeleteBuffers(1, &c.vbo);
        if (!c.textures.empty()) gl::DeleteTextures(static_cast<gl::GLsizei>(c.textures.size()), c.textures.data());
    }
    if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
    if (msFbo_) gl::DeleteFramebuffers(1, &msFbo_);
    if (msColour_) gl::DeleteRenderbuffers(1, &msColour_);
    if (msDepth_) gl::DeleteRenderbuffers(1, &msDepth_);
    if (colour_) gl::DeleteTextures(1, &colour_);
    if (depth_) gl::DeleteTextures(1, &depth_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    if (program_) gl::DeleteProgram(program_);
}

void BoxShelf::ensureTarget(int width, int height) {
    if (fbo_ && width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (!fbo_) {
        gl::GenFramebuffers(1, &fbo_);
        gl::GenTextures(1, &colour_);
        gl::GenTextures(1, &depth_);
    }
    gl::BindTexture(gl::TEXTURE_2D, colour_);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
    // Drawn at twice the size it is shown at: the linear filter averages it down smoothly.
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D, depth_);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::DEPTH_COMPONENT24, width, height, 0, gl::DEPTH_COMPONENT, gl::UNSIGNED_INT, nullptr);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::BindFramebuffer(gl::FRAMEBUFFER, fbo_);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, colour_, 0);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, depth_, 0);
    // The picture shrinks to the window through its mipmaps, averaged properly.
    gl::BindTexture(gl::TEXTURE_2D, colour_);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
    gl::GenerateMipmap(gl::TEXTURE_2D);
    if (!msFbo_) {
        gl::GenFramebuffers(1, &msFbo_);
        gl::GenRenderbuffers(1, &msColour_);
        gl::GenRenderbuffers(1, &msDepth_);
    }
    gl::BindRenderbuffer(gl::RENDERBUFFER, msColour_);
    gl::RenderbufferStorageMultisample(gl::RENDERBUFFER, 4, gl::RGBA8, width, height);
    gl::BindRenderbuffer(gl::RENDERBUFFER, msDepth_);
    gl::RenderbufferStorageMultisample(gl::RENDERBUFFER, 4, gl::DEPTH_COMPONENT24, width, height);
    gl::BindRenderbuffer(gl::RENDERBUFFER, 0);
    gl::BindFramebuffer(gl::FRAMEBUFFER, msFbo_);
    gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::RENDERBUFFER, msColour_);
    gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, msDepth_);
    multisample_ = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

void BoxShelf::reset() {
    state_ = State::Browse;
    take_ = open_ = eject_ = light_ = 0;
    boxTurn_ = cartTurn_ = kNoTurn;
    launch_ = launched_ = false;
    frozen_ = false;
    zoom_ = zoomShown_ = 1;
    read_ = turn_ = 0;
    spread_ = turnDir_ = 0;
    readPan_ = {};
}

void BoxShelf::pose(int step) {
    reset();
    if (step <= 0) return;
    take_ = 1;
    open_ = step >= 2 ? 1.0f : 0.0f;
    state_ = step == 1 ? State::Held : step == 2 ? State::Open : step == 3 ? State::Ejecting : step == 4 ? State::Inserted : State::Reading;
    eject_ = step == 3 ? 0.6f : step == 4 ? 1.0f : 0.0f;
    // 5: the manual out, at its first pages.
    if (step >= 5) read_ = 1, spread_ = 1;
    frozen_ = true;
}

void BoxShelf::activate() {
    if (count_ == 0) return;
    switch (state_) {
        case State::Browse:
        case State::Returning: state_ = State::Taking; break;
        case State::Held:
        case State::Closing: state_ = State::Opening; break;
        case State::Open:
            state_ = State::Ejecting;
            eject_ = 0;
            break;
        case State::Reading: turnPage(1); return;
        default: break;
    }
    zoom_ = 1;
}

bool BoxShelf::openManual() {
    if (state_ != State::Open || !manual_) return false;
    state_ = State::ManualOut;
    zoom_ = 1;
    readPan_ = {};
    spread_ = turnDir_ = 0;
    return true;
}

int BoxShelf::lastSpread() const { return static_cast<int>(pages_.size()) / 2; }

void BoxShelf::turnPage(int direction) {
    if (state_ != State::Reading || pages_.empty()) return;
    // A turn on its way ends at once, for quick presses.
    if (turnDir_) {
        spread_ += turnDir_;
        turnDir_ = 0;
    }
    const int to = spread_ + (direction > 0 ? 1 : -1);
    if (to < 0 || to > lastSpread()) return;
    turnDir_ = direction > 0 ? 1 : -1;
    turn_ = 0;
}

bool BoxShelf::side(int direction) {
    if (state_ == State::Reading) {
        turnPage(direction);
        return true;
    }
    if (state_ == State::Open && direction < 0) return openManual();
    return state_ != State::Browse;
}

void BoxShelf::setManual(std::vector<Page> pages) {
    pages_ = std::move(pages);
    if (!frozen_ && spread_ > lastSpread()) spread_ = lastSpread(), turnDir_ = 0;
}

void BoxShelf::manualPages(int* first, int* last) const {
    // The cover for the lid; around the pages open, the next ones and those before.
    if (state_ != State::ManualOut && state_ != State::Reading && state_ != State::ManualBack) {
        *first = 0;
        *last = 0;
        return;
    }
    const int s = spread_ + (turnDir_ > 0 ? 1 : 0);
    *first = std::max(0, 2 * s - 3);
    *last = 2 * s + 2;
}

bool BoxShelf::back() {
    switch (state_) {
        case State::Browse: return false;
        case State::Taking:
        case State::Held: state_ = State::Returning; zoom_ = 1; return true;
        case State::Opening:
        case State::Open: state_ = State::Closing; zoom_ = 1; return true;
        case State::ManualOut:
        case State::Reading: state_ = State::ManualBack; zoom_ = 1; return true;
        case State::ManualBack: return true;
        default: return true;  // on its way into the console
    }
}

BoxShelf::Stage BoxShelf::stage() const {
    switch (state_) {
        case State::Browse:
        case State::Returning: return Stage::Shelf;
        case State::Taking:
        case State::Held:
        case State::Closing: return Stage::Box;
        case State::Opening:
        case State::Open:
        case State::ManualBack: return Stage::Cartridge;
        case State::ManualOut:
        case State::Reading: return Stage::Manual;
        default: return Stage::Console;
    }
}

bool BoxShelf::takeLaunch() {
    if (!launch_ || launched_) return false;
    launched_ = true;
    return true;
}

int BoxShelf::click(float x, float y) {
    const Part* p = pick(x, y);
    switch (state_) {
        case State::Browse:
            if (!p || p->box < 0) return -1;
            selected_ = p->box;
            state_ = State::Taking;
            return p->box;
        case State::Held:
            if (p && p->box == selected_) activate();
            else back();
            return -1;
        case State::Open:
            if (p && p->manual) openManual();
            else if (p && p->cartridge) activate();
            else back();
            return -1;
        case State::Reading:
            // The right page goes on, the left one back.
            if (p && p->manual) turnPage(x >= 0.5f ? 1 : -1);
            else back();
            return -1;
        default: return -1;
    }
}

BoxShelf::Target BoxShelf::hover(float x, float y) {
    const Part* p = pick(x, y);
    switch (state_) {
        case State::Browse: return p && p->box >= 0 ? Target::Box : Target::None;
        case State::Held: return p && p->box == selected_ ? Target::Box : Target::None;
        case State::Open: return p && p->manual ? Target::Manual : p && p->cartridge ? Target::Cartridge : Target::None;
        case State::Reading: return p && p->manual ? Target::Page : Target::None;
        default: return Target::None;
    }
}

void BoxShelf::drag(float dx, float dy) {
    switch (state_) {
        case State::Held:
        case State::Open:
            turn(dx * 360, dy * 180);
            break;
        case State::Reading: {
            // Close up, the page follows the pointer.
            if (zoom_ <= 1.01f) break;
            float pw, ph;
            manualSize(&pw, &ph);
            const float across = 2 * std::max(2 * pw / aspect_, ph) * 1.08f / zoomShown_;
            readPan_.x = std::clamp(readPan_.x - dx * across * aspect_, -pw, pw);
            readPan_.y = std::clamp(readPan_.y + dy * across, -ph / 2, ph / 2);
            break;
        }
        case State::Browse: {
            // The shelf follows the pointer, as far as the picture is wide at its distance.
            const float across = 2 * distance_ * tanHalf_;
            pan_.x -= dx * across * aspect_;
            pan_.y += dy * across;
            break;
        }
        default: break;
    }
}

void BoxShelf::turn(float yaw, float pitch) {
    Quat* q = state_ == State::Held ? &boxTurn_ : state_ == State::Open ? &cartTurn_ : nullptr;
    if (!q || (yaw == 0 && pitch == 0)) return;
    // Around the screen's axes (the world's: the camera looks straight at what is out), so
    // each push turns it the same way on screen whatever side of it is showing.
    *q = normalized(quatY(yaw) * quatX(pitch) * *q);
}

void BoxShelf::zoom(float steps) {
    if (state_ == State::Browse) {
        distance_ *= std::pow(0.85f, steps);  // the most is set by render, once it knows the shelf
        return;
    }
    // On what is out in front: up to 4 times closer (5 on a page), a little further back.
    if (state_ == State::Held || state_ == State::Open || state_ == State::Reading)
        zoom_ = std::clamp(zoom_ / std::pow(0.85f, steps), 0.75f, state_ == State::Reading ? 5.0f : 4.0f);
    if (zoom_ <= 1.0f) readPan_ = {};
}

const BoxShelf::Part* BoxShelf::pick(float x, float y) const {
    if (width_ == 0) return nullptr;
    // The ray from the eye through the point, then against each part's box in its own space.
    const float nx = (2 * x - 1) * tanHalf_ * aspect_, ny = (1 - 2 * y) * tanHalf_;
    const Vec3 d{forward_.x + nx * right_.x + ny * up_.x, forward_.y + nx * right_.y + ny * up_.y,
                 forward_.z + nx * right_.z + ny * up_.z};
    const Part* best = nullptr;
    float bestT = 1e30f;
    for (const Part& p : parts_) {
        if ((p.box < 0 && !p.cartridge && !p.manual) || (p.cartridge && p.custom >= 0)) continue;
        const Mat4 inv = affineInverse(p.model);
        const float o[3] = {inv[0] * eye_.x + inv[4] * eye_.y + inv[8] * eye_.z + inv[12],
                            inv[1] * eye_.x + inv[5] * eye_.y + inv[9] * eye_.z + inv[13],
                            inv[2] * eye_.x + inv[6] * eye_.y + inv[10] * eye_.z + inv[14]};
        const float dir[3] = {inv[0] * d.x + inv[4] * d.y + inv[8] * d.z, inv[1] * d.x + inv[5] * d.y + inv[9] * d.z,
                              inv[2] * d.x + inv[6] * d.y + inv[10] * d.z};
        float t0 = 0, t1 = 1e30f;
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
            if (std::fabs(dir[a]) < 1e-9f) {
                hit = std::fabs(o[a]) <= 0.5f;
                continue;
            }
            float ta = (-0.5f - o[a]) / dir[a], tb = (0.5f - o[a]) / dir[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 <= t1;
        }
        // The cartridge and the manual win over the box they lie in.
        const float t = p.cartridge || p.manual ? t0 - 0.05f : t0;
        if (hit && t < bestT) {
            bestT = t;
            best = &p;
        }
    }
    return best;
}

void BoxShelf::camera(Vec3* eye, Vec3* target) const {
    const float a = smooth(take_), o = smooth(open_);
    const float e = state_ == State::Inserted ? 1.0f : state_ == State::Ejecting ? smooth(eject_ / 0.45f) : 0.0f;
    const float x = stageX_;
    // The shelf, the box out, its cartridge out and the console: each framed for its size.
    Vec3 consoleEye{0.15f, 0.55f, 4.3f}, consoleTarget{0.0f, -0.05f, 1.6f};  // a Game Boy, standing
    switch (system_) {
        case ConsoleSystem::GameBoyColor: consoleEye = {0.15f, 0.5f, 4.2f}, consoleTarget = {0.0f, -0.1f, 1.6f}; break;
        case ConsoleSystem::Nes: consoleEye = {0.5f, 1.3f, 5.6f}, consoleTarget = {0.0f, -0.35f, 1.7f}; break;
        case ConsoleSystem::Snes: consoleEye = {0.35f, 1.1f, 5.1f}, consoleTarget = {0.0f, -0.35f, 1.5f}; break;
        default: break;
    }
    const Vec3 shelfTarget = look_, shelfEye{look_.x, look_.y + distance_ * 0.1f, look_.z + distance_};
    Vec3 eyes[3] = {{x, 0.3f, 3.4f}, {x - 0.2f, 0.35f, 3.6f}, {x + consoleEye.x, consoleEye.y, consoleEye.z}};
    const Vec3 targets[3] = {{x, 0.05f, 0}, {x - 0.2f, 0.0f, 0.3f}, {x + consoleTarget.x, consoleTarget.y, consoleTarget.z}};
    // A narrow view steps back from the box and the console to keep them in.
    const float back = std::max(1.0f, 1.6f / aspect_);
    for (int i = 0; i < 3; ++i)
        eyes[i] = {targets[i].x + (eyes[i].x - targets[i].x) * back, targets[i].y + (eyes[i].y - targets[i].y) * back,
                   targets[i].z + (eyes[i].z - targets[i].z) * back};
    auto mix = [&](const Vec3& shelf, const Vec3 (&k)[3]) {
        const Vec3 b{lerp(k[0].x, k[1].x, o), lerp(k[0].y, k[1].y, o), lerp(k[0].z, k[1].z, o)};
        const Vec3 c{lerp(b.x, k[2].x, e), lerp(b.y, k[2].y, e), lerp(b.z, k[2].z, e)};
        return Vec3{lerp(shelf.x, c.x, a), lerp(shelf.y, c.y, a), lerp(shelf.z, c.z, a)};
    };
    *eye = mix(shelfEye, eyes);
    *target = mix(shelfTarget, targets);
    // Closer to what is out in front.
    if (take_ > 0 && zoomShown_ != 1) {
        const Vec3 t = *target;
        *eye = {t.x + (eye->x - t.x) / zoomShown_, t.y + (eye->y - t.y) / zoomShown_, t.z + (eye->z - t.z) / zoomShown_};
    }
    // The manual being read: its two pages fill the picture.
    const float r = smooth(read_);
    if (r > 0) {
        float pw, ph;
        manualSize(&pw, &ph);
        const Vec3 b = manualSpot();
        const float dist = std::max(2 * pw / aspect_, ph) * 1.08f / (2 * tanHalf_) / zoomShown_;
        const Vec3 t{b.x + bookShift_ + readPan_.x, b.y + readPan_.y, b.z};
        *eye = {lerp(eye->x, t.x, r), lerp(eye->y, t.y, r), lerp(eye->z, t.z + dist, r)};
        *target = {lerp(target->x, t.x, r), lerp(target->y, t.y, r), lerp(target->z, t.z, r)};
    }
}

void BoxShelf::manualSize(float* w, float* h) const {
    *h = held_.h * 0.88f;
    // As its pages are once they are known; until then, as the lid.
    float aspect = held_.w * 0.86f / *h;
    if (!pages_.empty() && pages_[0].width > 0 && pages_[0].height > 0)
        aspect = std::clamp(static_cast<float>(pages_[0].width) / static_cast<float>(pages_[0].height), 0.4f, 2.0f);
    *w = *h * aspect;
}

BoxShelf::Vec3 BoxShelf::manualSpot() const { return {stageX_ - 0.1f, 0.1f, 1.9f}; }

void BoxShelf::shelfSize(float* width, float* height, float* bottom) const {
    *width = rowWidth_ + 2 * kSide + 0.06f;
    *height = kPlinth + static_cast<float>(rows_) * kRowH + kBoard;
    *bottom = kFloorY;
}

BoxShelf::Vec3 BoxShelf::shelfSpot(int index) const {
    const int c = index % columns_, r = index / columns_;
    const bool known = index >= 0 && index < static_cast<int>(spotX_.size());
    const float x = known ? spotX_[static_cast<size_t>(index)] : (static_cast<float>(c) - static_cast<float>(columns_ - 1) / 2) * kSpacing;
    const float h = known ? sizes_[static_cast<size_t>(index)].h : kBoxH;
    // Standing on its board, whatever its height.
    return {x, kFloorY + kPlinth + static_cast<float>(rows_ - 1 - r) * kRowH + h / 2 + 0.002f, kShelfZ};
}

namespace {

// The front of each system's box, width / height: the NES's (12.7 x 18.2 cm), the Game
// Boy's (10.2 x 12.7 cm), the Super Nintendo's lying on its side (18 x 12.7 cm).
float frontAspect(ConsoleSystem s) {
    switch (s) {
        case ConsoleSystem::Nes: return 12.7f / 18.2f;
        case ConsoleSystem::Snes: return 18.0f / 12.7f;
        default: return 10.2f / 12.7f;
    }
}

}  // namespace

BoxShelf::Size BoxShelf::boxSize(const Item& item) {
    Size s;
    switch (item.system) {
        case ConsoleSystem::Nes: s = {12.7f / 13.5f, 18.2f / 13.5f, 3.8f / 13.5f}; break;
        case ConsoleSystem::Snes: s = {18.0f / 13.5f, 12.7f / 13.5f, 3.8f / 13.5f}; break;
        default: s = {10.2f / 13.5f, 12.7f / 13.5f, 2.5f / 13.5f}; break;
    }
    // A Super Nintendo cover taller than wide (a European one standing up) gets a box to match.
    if (item.system == ConsoleSystem::Snes && item.texture && item.width > 0 && item.height > 0) {
        const float w = static_cast<float>(item.width), h = static_cast<float>(item.height);
        const float f = coverFront(item.width, item.height, item.system);
        if ((f > 0 ? w * (1 - f) : w) < h) std::swap(s.w, s.h);
    }
    return s;
}

BoxShelf::Mat4 BoxShelf::heldBox() const {
    // Out of the shelf towards the viewer, then over to the front, turning to show its
    // cover; aside to the left once its cartridge is out.
    const Vec3 s = shelfSpot(selected_);
    const float t1 = smooth(take_ / 0.4f), t2 = smooth((take_ - 0.3f) / 0.7f);
    const Vec3 out{s.x, s.y, kShelfZ + 1.1f}, front{stageX_ - 0.35f * smooth(open_), 0.1f, 0};
    const Vec3 a{lerp(s.x, out.x, t1), lerp(s.y, out.y, t1), lerp(s.z, out.z, t1)};
    const Vec3 p{lerp(a.x, front.x, t2), lerp(a.y, front.y, t2), lerp(a.z, front.z, t2)};
    return translate(p.x, p.y, p.z) * rotateY(90 * (1 - t2)) * rotation(boxTurn_);
}

BoxShelf::Mat4 BoxShelf::heldCartridge() const {
    // Lifted out of the box and up in front of it, facing the viewer.
    const Mat4 in = cartridgeInBox(heldBox(), system_, held_);
    const float s = smooth((open_ - 0.35f) / 0.65f);
    if (s <= 0) return in;
    const Vec3 out{stageX_ + 0.45f, 0.1f, 1.3f};
    const Vec3 p{lerp(in[12], out.x, s), lerp(in[13], out.y, s) + std::sin(s * kPi) * 0.25f, lerp(in[14], out.z, s)};
    return translate(p.x, p.y, p.z) * rotation(towards(kNoTurn, cartTurn_, s));
}

BoxShelf::Mat4 BoxShelf::cartridgeInBox(const Mat4& box, ConsoleSystem s, const Size& size) const {
    // The cartridge lies in the lower half, its label to the front and its contacts down.
    const CartridgeShape c = cartridgeShape(s);
    return box * translate(0, -size.h / 2 + kWall + 0.1f + c.h / 2, -size.d / 2 + kWall + 0.006f + c.d / 2);
}

float BoxShelf::coverFront(int width, int height, ConsoleSystem system) {
    if (width <= 0 || height <= 0) return 0;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    // A whole sleeve is the back, the spine and the front side by side, so it is told by
    // being well wider than a front.
    const float aspect = frontAspect(system);
    if (w < h * aspect * 1.6f) return 0;
    return std::clamp(1 - aspect * h / w, 0.0f, 0.9f);
}

namespace {

// The front, spine and back of a cover as texture rectangles; the back and spine are
// empty (u1 <= u0) on a cover that is only the front.
struct Sleeve {
    std::array<float, 4> front{0, 0, 1, 1}, spine{0, 0, 0, 1}, back{0, 0, 0, 1};
};

Sleeve sleeve(int width, int height, ConsoleSystem system) {
    Sleeve s;
    const float f = BoxShelf::coverFront(width, height, system);
    if (f <= 0) return s;
    const float side = 1 - f;  // the front's width, and the back's
    s.front = {f, 0, 1, 1};
    s.back = {0, 0, side, 1};
    s.spine = {side, 0, f, 1};
    return s;
}

}  // namespace

void BoxShelf::addCartridge(const Item& item, const Mat4& place) {
    Part p;
    p.cartridge = true;
    // Its own label, or else the cover's front.
    const unsigned int art = item.label ? item.label : item.texture;
    p.texRect = item.label ? std::array<float, 4>{0, 0, 1, 1} : sleeve(item.width, item.height, item.system).front;
    const CartridgeShape c = cartridgeShape(item.system);
    // A cartridge's own model, with the game's label over the one it has.
    const int ownIndex = kCartCustom + static_cast<int>(item.system);
    const Custom& own = custom_[ownIndex];
    if (own.vao) {
        p.model = place * own.fit;
        p.colour = {1, 1, 1};
        p.shine = 0.35f;
        p.custom = ownIndex;
        p.label = art;
        p.labelRect = p.texRect;
        p.labelWhole = item.label != 0;
        // A cover's middle, cut to the picture's shape.
        if (!p.labelWhole && item.width > 0 && item.height > 0) {
            std::array<float, 4>& r = p.labelRect;
            const float w = (r[2] - r[0]) * static_cast<float>(item.width);
            const float h = std::min(w / own.pictureAspect / static_cast<float>(item.height), r[3] - r[1]);
            const float mid = r[1] + (r[3] - r[1]) * own.pictureMiddle;
            r[1] = std::clamp(mid - h / 2, r[1], r[3] - h);
            r[3] = r[1] + h;
        }
        p.texRect = {0, 0, 1, 1};
        p.reflect = true;
        parts_.push_back(p);
        Part hit;
        hit.model = place * scale(c.w, c.h, c.d);
        hit.cartridge = true;
        hit.hidden = true;
        parts_.push_back(hit);
        return;
    }
    p.model = place;
    p.colour = {1, 1, 1};
    p.shine = 0.35f;
    p.mesh = kCartMeshes + static_cast<int>(item.system);
    parts_.push_back(p);
    // The label (the cover) in its recess.
    p.mesh = -1;
    p.model = place * translate(0, c.labelY, c.d / 2 + 0.0025f) * scale(c.labelW, c.labelH, 0.001f);
    p.colour = art ? Vec3{1, 1, 1} : Vec3{0.75f, 0.72f, 0.66f};
    p.shine = 0.15f;
    p.texture = art;
    p.texFaces = art ? 1 << 4 : 0;
    parts_.push_back(p);
}

BoxShelf::Mat4 BoxShelf::addBox(const std::vector<Item>& items, int index, const Mat4& place, float lid, bool withManual) {
    const Item& item = items[static_cast<size_t>(index)];
    const Size S = boxSize(item);
    const float bw = S.w, bh = S.h, bd = S.d;  // this box's own
    auto add = [&](const Mat4& m, Vec3 colour, float shine, int texFaces = 0) {
        Part p;
        p.model = place * m;
        p.colour = colour;
        p.shine = shine;
        p.texture = item.texture;
        p.texFaces = item.texture ? texFaces : 0;
        p.box = index;
        parts_.push_back(p);
    };
    const Vec3 plastic{0.045f, 0.045f, 0.055f};
    const float inner = bd - kWall;
    // With a whole sleeve its back and spine go round the tray.
    const Sleeve cover = sleeve(item.width, item.height, item.system);
    const bool wrap = cover.spine[2] > cover.spine[0];
    // The tray: its back and four walls, open at the front.
    add(translate(0, 0, -bd / 2 + kWall / 2) * scale(bw, bh, kWall), plastic, 0.6f, wrap ? 1 << 5 : 0);
    parts_.back().texRect = cover.back;
    add(translate(-bw / 2 + kWall / 2, 0, -kWall / 2) * scale(kWall, bh, inner), plastic, 0.6f, wrap ? 1 << 1 : 0);
    parts_.back().texRect = cover.spine;
    add(translate(bw / 2 - kWall / 2, 0, -kWall / 2) * scale(kWall, bh, inner), plastic, 0.6f);
    add(translate(0, bh / 2 - kWall / 2, -kWall / 2) * scale(bw - 2 * kWall, kWall, inner), plastic, 0.6f);
    add(translate(0, -bh / 2 + kWall / 2, -kWall / 2) * scale(bw - 2 * kWall, kWall, inner), plastic, 0.6f);
    // Without a cover the lid takes the console's colour.
    Vec3 lidColour{0.6f, 0.6f, 0.57f};
    switch (item.system) {
        case ConsoleSystem::GameBoyColor: lidColour = {0.08f, 0.45f, 0.45f}; break;
        case ConsoleSystem::Nes: lidColour = {0.2f, 0.2f, 0.22f}; break;
        case ConsoleSystem::Snes: lidColour = {0.5f, 0.5f, 0.56f}; break;
        default: break;
    }
    const Mat4 hinge = translate(-bw / 2, 0, bd / 2 - kWall / 2) * rotateY(-lid * kLidOpen);
    add(hinge * translate(bw / 2, 0, 0) * scale(bw, bh, kWall), item.texture ? plastic : lidColour, 0.7f, 1 << 4);
    parts_.back().texRect = cover.front;
    const Mat4 manual = hinge * translate(bw / 2, 0, -kWall / 2 - 0.004f);
    if (lid <= 0.001f) return place * manual;
    // Inside: the manual on the lid (its own cover once it is drawn), the holder and the game.
    if (withManual) {
        add(manual * scale(bw * 0.86f, bh * 0.88f, 0.006f), {0.86f, 0.84f, 0.78f}, 0.1f, 1 << 5);
        parts_.back().texRect = cover.front;
        if (item.manual && index == selected_) {
            parts_.back().manual = true;
            if (!pages_.empty() && pages_[0].texture) {
                parts_.back().texture = pages_[0].texture;
                parts_.back().texFaces = 1 << 5;
                parts_.back().texRect = {0, 0, 1, 1};
                parts_.back().colour = {1, 1, 1};
            }
        }
    }
    add(translate(0, 0, -bd / 2 + kWall + 0.003f) * scale(bw - 2 * kWall, bh - 2 * kWall, 0.006f),
        {0.11f, 0.11f, 0.13f}, 0.3f);
    return place * manual;
}

BoxShelf::Mat4 BoxShelf::addConsole(const Item& item, float light, float rise) {
    using L = ConsoleLayout;
    const ConsoleSystem s = item.system;
    const ConsoleModel m = consoleModelOf(s);
    const Mat4 base = translate(stageX_, kFloorY + rise, kConsoleZ);
    // The console's own model when it was given one, else the built-in.
    const Custom& own = custom_[static_cast<int>(m)];
    Part p;
    p.colour = {1, 1, 1};
    p.shine = 0.45f;
    if (own.vao) {
        p.model = base * own.fit;
        p.custom = static_cast<int>(m);
    } else {
        p.model = base;
        p.mesh = static_cast<int>(m);
    }
    parts_.push_back(p);
    // The power light: dark red on the built-in model, lit once the game is in.
    const Led l = ledOf(m);
    if (light > 0 || !own.vao) {
        Part led;
        led.model = base * translate(l.at[0], l.at[1], l.at[2]) * rotateX(l.tilt - 90) * scale(0.03f, 0.03f, 0.004f);
        led.colour = light > 0 ? Vec3{1.0f, 0.12f, 0.08f} : Vec3{0.22f, 0.03f, 0.03f};
        led.glow = light > 0 ? 1.6f : 0.0f;
        led.shine = 0.6f;
        led.cylinder = true;
        parts_.push_back(led);
    }
    const CartridgeShape c = cartridgeShape(s);
    switch (s) {
        case ConsoleSystem::Nes:
            // Lying on its back in the door, pushed down: its label up, its top end out.
            return translate(L::kNesDoor[0], kFloorY + L::kNesDoor[1] - L::kNesDown, kConsoleZ + L::kNesDoor[2] - L::kNesIn) *
                   rotateY(180) * rotateX(-90);
        case ConsoleSystem::Snes:
            return translate(L::kSnesSlot[0], kFloorY + L::kSnesSlot[1] + c.h / 2 - c.insert, kConsoleZ + L::kSnesSlot[2]);
        default: {
            // A Game Boy's screen shows the game's cover once it is on.
            const bool color = s == ConsoleSystem::GameBoyColor;
            const float* sc = color ? L::kGbcScreen : L::kGbScreen;
            const float* slot = color ? L::kGbcSlot : L::kGbSlot;
            Part screen;
            screen.model = base * translate(sc[0], sc[1], (color ? L::kGbcFrontZ : L::kGbFrontZ) + 0.002f) * scale(sc[2], sc[3], 0.002f);
            screen.colour = color ? Vec3{0.05f, 0.06f, 0.05f} : Vec3{0.42f, 0.47f, 0.2f};
            screen.shine = 1.0f;
            if (light > 0 && item.texture) {
                screen.texture = item.texture;
                screen.texFaces = 1 << 4;
                screen.texRect = sleeve(item.width, item.height, item.system).front;
                screen.colour = {1, 1, 1};
                screen.glow = 0.6f;
            }
            parts_.push_back(screen);
            return translate(slot[0], kFloorY + slot[1] + c.h / 2 - c.insert, kConsoleZ + slot[2]);
        }
    }
}

void BoxShelf::addShelfBox(const Item& item, int index, const Mat4& place) {
    // Closed, only its spine shows: the sleeve's, or the edge of the cover next to it.
    Part p;
    const Size S = index < static_cast<int>(sizes_.size()) ? sizes_[static_cast<size_t>(index)] : Size{};
    const float bw = S.w, bd = S.d;
    p.model = place * scale(S.w, S.h, S.d);
    p.colour = {0.045f, 0.045f, 0.055f};
    p.shine = 0.6f;
    p.box = index;
    if (item.texture) {
        const Sleeve cover = sleeve(item.width, item.height, item.system);
        p.texture = item.texture;
        p.texFaces = 1 << 1;
        if (cover.spine[2] > cover.spine[0]) p.texRect = cover.spine;
        else p.texRect = {cover.front[0], cover.front[1], cover.front[0] + (cover.front[2] - cover.front[0]) * bd / bw,
                          cover.front[3]};
    }
    parts_.push_back(p);
}

void BoxShelf::addShelf() {
    // Dark wood: a board under each row and over the top one, the sides and the back.
    float w, h, bottom;
    shelfSize(&w, &h, &bottom);
    const float depth = kBoxW + 0.12f, z = kShelfZ - 0.03f;
    auto board = [&](float x, float y, float zz, float sx, float sy, float sz, Vec3 colour) {
        Part p;
        p.model = translate(x, y, zz) * scale(sx, sy, sz);
        p.colour = colour;
        p.shine = 0.25f;
        parts_.push_back(p);
    };
    const Vec3 wood{0.17f, 0.1f, 0.06f}, inside{0.11f, 0.065f, 0.04f};
    board(0, bottom + kPlinth / 2, z, w, kPlinth, depth, wood);
    for (int r = 1; r <= rows_; ++r)
        board(0, bottom + kPlinth + static_cast<float>(r) * kRowH - kBoard / 2, z, w, kBoard, depth, wood);
    board(-w / 2 + kSide / 2, bottom + h / 2, z, kSide, h, depth, wood);
    board(w / 2 - kSide / 2, bottom + h / 2, z, kSide, h, depth, wood);
    board(0, bottom + h / 2, z - depth / 2 + 0.01f, w - 2 * kSide, h, 0.02f, inside);
}

void BoxShelf::addSelectionFrame(const Vec3& s, const Size& size) {
    const float bw = size.w, bh = size.h, bd = size.d;
    // A frame that lights itself around the chosen box's spine, beating slowly, so it can
    // be told apart from far away too.
    const float beat = 0.5f + 0.5f * std::sin(glowClock_ * 2 * kPi / 1.2f);
    // Thicker as the camera steps back, to stay about as wide on screen.
    const float bar = 0.016f * std::max(1.0f, distance_ / 2.5f), out = bar * 0.75f, front = s.z + bw / 2 + 0.004f;
    const float w = bd + 2 * out, h = bh + 2 * out;
    auto add = [&](float x, float y, float sx, float sy) {
        Part p;
        p.model = translate(x, y, front) * scale(sx, sy, 0.02f);
        p.colour = {1.0f, 0.72f, 0.12f};
        p.glow = 1.2f + 1.3f * beat;
        p.shine = 0.1f;
        parts_.push_back(p);
    };
    add(s.x, s.y + h / 2 - bar / 2, w, bar);
    add(s.x, s.y - h / 2 + bar / 2, w, bar);
    add(s.x - w / 2 + bar / 2, s.y, bar, h);
    add(s.x + w / 2 - bar / 2, s.y, bar, h);
}

void BoxShelf::build(const std::vector<Item>& items, int selected) {
    parts_.clear();
    addShelf();
    addRoom();
    for (int i = 0; i < count_; ++i) {
        if (i == selected && state_ != State::Browse) continue;
        Vec3 s = shelfSpot(i);
        if (i == pulled_) s.z += 0.14f * smooth(pull_);
        addShelfBox(items[static_cast<size_t>(i)], i, translate(s.x, s.y, s.z) * rotateY(90));
        if (i == selected && state_ == State::Browse) addSelectionFrame(s, sizes_[static_cast<size_t>(i)]);
    }
    if (state_ == State::Browse || selected < 0 || selected >= count_) return;
    const Item& item = items[static_cast<size_t>(selected)];
    const Mat4 box = heldBox();
    const float lid = smooth(open_ / 0.6f);
    const bool reading = state_ == State::ManualOut || state_ == State::Reading || state_ == State::ManualBack;
    const Mat4 lidManual = addBox(items, selected, box, lid, !reading);
    const bool ejecting = state_ == State::Ejecting || state_ == State::Inserted;
    if (!ejecting) {
        if (lid > 0.001f) addCartridge(item, heldCartridge());
        if (reading) addManual(item, lidManual);
        return;
    }
    // The console comes up through the floor while the cartridge leaves the box.
    const float light = state_ == State::Inserted ? 1.0f : 0.0f;
    const float t = state_ == State::Inserted ? 1.0f : eject_;
    const float rise = -1.6f * (1 - smooth(t / 0.3f));
    const Mat4 inConsole = translate(stageX_, 0, 0) * addConsole(item, light, rise);
    // The cartridge's way: up from where it was held, over to above the slot in an arc,
    // then down into it. Into an NES it goes lying on its back, in through the door, and
    // then it is pushed down.
    const bool nes = system_ == ConsoleSystem::Nes;
    const Mat4 start = heldCartridge();
    const Vec3 p0{start[12], start[13], start[14]}, p1{p0.x, p0.y + 0.15f, p0.z};
    const Vec3 p3{inConsole[12], inConsole[13], inConsole[14]};
    const Vec3 in{p3.x, p3.y + (nes ? ConsoleLayout::kNesDown : 0.0f), p3.z};
    const Vec3 p2 = nes ? Vec3{in.x, in.y, in.z + ConsoleLayout::kNesIn + cartridgeShape(system_).h / 2 + 0.08f}
                        : Vec3{p3.x, p3.y + cartridgeShape(system_).insert + 0.12f, p3.z};
    Vec3 p;
    if (t < 0.25f) {
        const float s = smooth(t / 0.25f);
        p = {lerp(p0.x, p1.x, s), lerp(p0.y, p1.y, s), lerp(p0.z, p1.z, s)};
    } else if (t < 0.7f) {
        const float s = smooth((t - 0.25f) / 0.45f);
        p = {lerp(p1.x, p2.x, s), lerp(p1.y, p2.y, s) + std::sin(s * kPi) * 0.2f, lerp(p1.z, p2.z, s)};
    } else if (nes) {
        // In, then down.
        const float s = smooth((t - 0.7f) / 0.2f), d = smooth((t - 0.9f) / 0.1f);
        p = {in.x, lerp(in.y, p3.y, d), lerp(p2.z, in.z, s)};
    } else {
        const float s = smooth((t - 0.7f) / 0.3f);
        p = {p3.x, lerp(p2.y, p3.y, s), p3.z};
    }
    // Into an NES on its back, its top end to the viewer; into a Game Boy with its label to
    // the back, the top of it showing.
    const float lay = nes ? -90.0f * smooth((t - 0.25f) / 0.45f) : 0.0f;
    const bool handheld = system_ == ConsoleSystem::GameBoy || system_ == ConsoleSystem::GameBoyColor;
    const float spin = nes || handheld ? 180.0f * smooth((t - 0.25f) / 0.45f) : 0.0f;
    addCartridge(item, translate(p.x, p.y, p.z) * rotateY(spin) * rotation(cartTurn_) * rotateX(lay));
}

void BoxShelf::addManual(const Item& item, const Mat4& lidManual) {
    float pw, ph;
    manualSize(&pw, &ph);
    const Vec3 b = manualSpot();
    const int count = static_cast<int>(pages_.size());
    auto page = [&](int i) { return i >= 0 && i < count ? pages_[static_cast<size_t>(i)] : Page{}; };
    // A sheet of paper: its page on the front (face 4) or the back (face 5), white until drawn.
    auto sheet = [&](const Mat4& m, int index, int face) {
        Part p;
        p.model = m;
        p.manual = true;
        p.reflect = false;
        p.shine = 0.04f;
        p.glow = 0.06f;
        const Page pg = page(index);
        p.colour = pg.texture ? Vec3{1, 1, 1} : Vec3{0.93f, 0.92f, 0.88f};
        p.texture = pg.texture;
        p.texFaces = pg.texture ? 1 << face : 0;
        parts_.push_back(p);
    };
    const float r = smooth(read_);
    if (state_ != State::Reading) {
        // Closed, from the lid to the right of where its spine will be, its cover to the front.
        const float lw = held_.w * 0.86f, lh = held_.h * 0.88f;
        const Vec3 to{b.x + pw / 2, b.y, b.z};
        const Vec3 p{lerp(lidManual[12], to.x, r), lerp(lidManual[13], to.y, r) + std::sin(r * kPi) * 0.15f,
                     lerp(lidManual[14], to.z, r)};
        const Mat4 m = translate(p.x, p.y, p.z) * rotateY(lerp(-kLidOpen, -180.0f, r)) *
                       scale(lerp(lw, pw, r), lerp(lh, ph, r), 0.006f);
        if (page(0).texture) sheet(m, 0, 5);
        else {
            // Not drawn yet: as it looked in the lid.
            Part p;
            p.model = m;
            p.manual = true;
            p.colour = {0.86f, 0.84f, 0.78f};
            p.shine = 0.1f;
            p.texture = item.texture;
            p.texFaces = item.texture ? 1 << 5 : 0;
            p.texRect = sleeve(item.width, item.height, item.system).front;
            parts_.push_back(p);
        }
        return;
    }
    // Open at spread_: page 2s-1 on the left, 2s on the right. A page turning forwards
    // shows the next ones under it; backwards, the ones before.
    int left = 2 * spread_ - 1, right = 2 * spread_;
    if (turnDir_ > 0) right += 2;
    if (turnDir_ < 0) left -= 2;
    const float paper = 0.004f;
    auto flat = [&](int index, float side) {
        if (index < 0 || index >= count) return;
        // The cover's board under it, a little bigger.
        Part back;
        back.model = translate(b.x + side * (pw / 2 + 0.006f), b.y, b.z - paper) * scale(pw + 0.012f, ph + 0.016f, paper);
        back.colour = {0.12f, 0.12f, 0.14f};
        back.shine = 0.2f;
        back.reflect = false;
        parts_.push_back(back);
        sheet(translate(b.x + side * pw / 2, b.y, b.z) * scale(pw, ph, paper), index, 4);
    };
    flat(left, -1);
    flat(right, 1);
    if (turnDir_ == 0) return;
    // The leaf over the spine: its front was the right page, its back the left one.
    const int front = turnDir_ > 0 ? 2 * spread_ : 2 * spread_ - 2;
    const float t = smooth(turn_), angle = 180 * (turnDir_ > 0 ? t : 1 - t);
    const Mat4 leaf = translate(b.x, b.y, b.z + paper * 1.5f) * rotateY(-angle) * translate(pw / 2, 0, 0);
    sheet(leaf * translate(0, 0, 0.0006f) * scale(pw, ph, 0.001f), front, 4);
    sheet(leaf * translate(0, 0, -0.0006f) * scale(pw, ph, 0.001f), front + 1, 5);
}

unsigned int BoxShelf::render(const std::vector<Item>& items, int selected, int width, int height, float dt) {
    if (!program_ || width <= 0 || height <= 0) return 0;
    dt = std::clamp(dt, 0.0f, 0.1f);
    count_ = static_cast<int>(items.size());
    if (selected != selected_ && state_ != State::Browse && !frozen_) reset();
    if (selected != selected_) pan_ = {};  // chosen with the keys: the camera goes to it
    selected_ = std::clamp(selected, 0, std::max(0, count_ - 1));
    if (count_ > 0) system_ = items[static_cast<size_t>(selected_)].system;
    else state_ = State::Browse;
    // The shelf about half again as wide as tall, but never fewer than 8 boxes a row.
    const float n = static_cast<float>(std::max(1, count_));
    columns_ = std::clamp(static_cast<int>(std::lround(std::sqrt(n * kRowH * 1.6f / kSpacing))), std::min(count_, 8), std::max(1, count_));
    columns_ = std::max(1, columns_);
    rows_ = std::max(1, (count_ + columns_ - 1) / columns_);
    // Each box's size, and the rows laid out with them side by side.
    sizes_.resize(static_cast<size_t>(count_));
    spotX_.resize(static_cast<size_t>(count_));
    for (int i = 0; i < count_; ++i) sizes_[static_cast<size_t>(i)] = boxSize(items[static_cast<size_t>(i)]);
    rowWidth_ = 0;
    for (int first = 0; first < count_; first += columns_) {
        const int last = std::min(count_, first + columns_);
        float w = 0;
        for (int i = first; i < last; ++i) w += sizes_[static_cast<size_t>(i)].d + (kSpacing - kBoxD);
        // A row of Mega Drive boxes keeps its old spacing; the shelf is as wide as a full one.
        rowWidth_ = std::max(rowWidth_, w);
        float x = -w / 2;
        for (int i = first; i < last; ++i) {
            const float step = sizes_[static_cast<size_t>(i)].d + (kSpacing - kBoxD);
            spotX_[static_cast<size_t>(i)] = x + step / 2;
            x += step;
        }
    }
    held_ = count_ > 0 ? sizes_[static_cast<size_t>(selected_)] : Size{};
    manual_ = count_ > 0 && items[static_cast<size_t>(selected_)].manual;
    if (!manual_ && (state_ == State::ManualOut || state_ == State::Reading)) state_ = State::ManualBack;
    if (state_ == State::Browse) stageX_ = shelfSpot(selected_).x;
    if (pulled_ != selected_) pulled_ = selected_, pull_ = 0;
    pull_ = std::min(1.0f, pull_ + dt / 0.25f);
    glowClock_ = std::fmod(glowClock_ + dt, 1200.0f);
    switch (frozen_ ? State::Browse : state_) {
        case State::Taking:
            take_ = std::min(1.0f, take_ + dt / 0.9f);
            if (take_ >= 1) state_ = State::Held;
            break;
        case State::Returning:
            take_ = std::max(0.0f, take_ - dt / 0.7f);
            if (take_ <= 0) state_ = State::Browse;
            break;
        case State::Opening:
            open_ = std::min(1.0f, open_ + dt / 0.9f);
            if (open_ >= 1) state_ = State::Open;
            break;
        case State::Closing:
            open_ = std::max(0.0f, open_ - dt / 0.7f);
            if (open_ <= 0) state_ = State::Held;
            break;
        case State::Ejecting:
            eject_ = std::min(1.0f, eject_ + dt / 2.2f);
            if (eject_ >= 1) {
                state_ = State::Inserted;
                light_ = 0;
            }
            break;
        case State::Inserted:
            light_ += dt;
            if (light_ > 0.8f) launch_ = true;
            break;
        case State::ManualOut:
            read_ = std::min(1.0f, read_ + dt / 0.8f);
            if (read_ >= 1) state_ = State::Reading;
            break;
        case State::ManualBack:
            read_ = std::max(0.0f, read_ - dt / 0.6f);
            if (read_ <= 0) {
                state_ = State::Open;
                turnDir_ = 0;
                readPan_ = {};
            }
            break;
        case State::Reading:
            if (turnDir_) {
                turn_ = std::min(1.0f, turn_ + dt / 0.45f);
                if (turn_ >= 1) {
                    spread_ += turnDir_;
                    turnDir_ = 0;
                }
            }
            break;
        default: break;
    }
    // The camera eases to the zoom asked for; on a closed cover or a last page alone it
    // frames that page instead of the spine.
    {
        const float k = 1 - std::exp(-12 * dt);
        zoomShown_ += (zoom_ - zoomShown_) * (frozen_ ? 1 : k);
        float pw, ph;
        manualSize(&pw, &ph);
        const int s = spread_ + turnDir_;
        const float shift = s == 0 ? pw / 2 : 2 * s >= static_cast<int>(pages_.size()) ? -pw / 2 : 0.0f;
        bookShift_ += (shift - bookShift_) * (frozen_ || state_ != State::Reading ? 1 : k * 0.5f);
    }
    // What is no longer held turns back to face the viewer.
    auto settle = [&](Quat& q) { q = towards(kNoTurn, q, std::exp(-7 * dt)); };
    if (state_ != State::Held && !frozen_) settle(boxTurn_);
    if (state_ != State::Open && state_ != State::Reading && state_ != State::ManualOut && state_ != State::ManualBack && !frozen_)
        settle(cartTurn_);

    ensureTarget(width, height);
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    tanHalf_ = std::tan(kFov * kRad / 2);
    // The shelf's camera: always on the chosen box (and as far as it was dragged), at any
    // distance; stepping back goes no further than to see all of the shelf around it.
    float w, h, bottom;
    shelfSize(&w, &h, &bottom);
    const Vec3 spot = shelfSpot(selected_);
    const float spanX = 2 * std::max(spot.x + w / 2, w / 2 - spot.x);
    const float spanY = 2 * std::max(spot.y - bottom, bottom + h - spot.y);
    const float fit = std::max(spanX / aspect_, spanY) / (2 * tanHalf_) * 1.08f + 0.6f;
    const float nearest = 1.6f;
    distance_ = std::clamp(distance_, nearest, std::max(fit, 3.0f));
    Vec3 goal{spot.x + pan_.x, spot.y + pan_.y, kShelfZ + 0.5f};
    goal.x = std::clamp(goal.x, -w / 2, w / 2);
    goal.y = std::clamp(goal.y, bottom + 0.5f, bottom + std::max(h - 0.5f, 0.5f));
    if (!placed_) look_ = goal;
    placed_ = true;
    const float follow = 1 - std::exp(-10 * dt);
    look_ = {look_.x + (goal.x - look_.x) * follow, look_.y + (goal.y - look_.y) * follow, goal.z};
    build(items, selected_);
    Vec3 target;
    camera(&eye_, &target);
    // The camera's axes, for clicks, and its matrix.
    forward_ = {target.x - eye_.x, target.y - eye_.y, target.z - eye_.z};
    const float fl = std::sqrt(forward_.x * forward_.x + forward_.y * forward_.y + forward_.z * forward_.z);
    forward_ = {forward_.x / fl, forward_.y / fl, forward_.z / fl};
    right_ = {-forward_.z, 0, forward_.x};  // forward x (0, 1, 0)
    const float rl = std::sqrt(right_.x * right_.x + right_.z * right_.z);
    right_ = {right_.x / rl, 0, right_.z / rl};
    up_ = {right_.y * forward_.z - right_.z * forward_.y, right_.z * forward_.x - right_.x * forward_.z,
           right_.x * forward_.y - right_.y * forward_.x};
    Mat4 view = identity();
    view[0] = right_.x, view[4] = right_.y, view[8] = right_.z;
    view[1] = up_.x, view[5] = up_.y, view[9] = up_.z;
    view[2] = -forward_.x, view[6] = -forward_.y, view[10] = -forward_.z;
    view[12] = -(right_.x * eye_.x + right_.y * eye_.y + right_.z * eye_.z);
    view[13] = -(up_.x * eye_.x + up_.y * eye_.y + up_.z * eye_.z);
    view[14] = forward_.x * eye_.x + forward_.y * eye_.y + forward_.z * eye_.z;
    viewProj_ = perspective(kFov * kRad, aspect_, 0.1f, 150.0f) * view;

    gl::BindFramebuffer(gl::FRAMEBUFFER, multisample_ ? msFbo_ : fbo_);
    gl::Viewport(0, 0, width, height);
    gl::Disable(gl::SCISSOR_TEST);
    gl::Disable(gl::CULL_FACE);
    gl::Disable(gl::BLEND);
    gl::Enable(gl::DEPTH_TEST);
    gl::DepthFunc(gl::LEQUAL);
    gl::DepthMask(gl::TRUE_);
    gl::ClearColor(kBackground[0], kBackground[1], kBackground[2], 1);
    gl::Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    gl::UseProgram(program_);
    gl::BindVertexArray(vao_);
    gl::ActiveTexture(gl::TEXTURE0);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uImage"), 0);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uNormals"), 1);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uLabel"), 2);
    const gl::GLint uHasLabel = gl::GetUniformLocation(program_, "uHasLabel");
    const gl::GLint uLabelArea = gl::GetUniformLocation(program_, "uLabelArea");
    const gl::GLint uLabelRect = gl::GetUniformLocation(program_, "uLabelRect");
    const gl::GLint uLabelTurn = gl::GetUniformLocation(program_, "uLabelTurn");
    const gl::GLint uHasNormals = gl::GetUniformLocation(program_, "uHasNormals");
    const gl::GLint uMask = gl::GetUniformLocation(program_, "uMask");
    gl::Uniform1i(uMask, 0);
    gl::UniformMatrix4fv(gl::GetUniformLocation(program_, "uViewProj"), 1, gl::FALSE_, viewProj_.data());
    gl::Uniform3f(gl::GetUniformLocation(program_, "uEye"), eye_.x, eye_.y, eye_.z);
    gl::Uniform1f(gl::GetUniformLocation(program_, "uFloorY"), kFloorY);
    const gl::GLint uModel = gl::GetUniformLocation(program_, "uModel");
    const gl::GLint uColour = gl::GetUniformLocation(program_, "uColour");
    const gl::GLint uTexFaces = gl::GetUniformLocation(program_, "uTexFaces");
    const gl::GLint uTexRect = gl::GetUniformLocation(program_, "uTexRect");
    const gl::GLint uGlow = gl::GetUniformLocation(program_, "uGlow");
    const gl::GLint uShine = gl::GetUniformLocation(program_, "uShine");
    const gl::GLint uMirror = gl::GetUniformLocation(program_, "uMirror");
    const gl::GLint uFloor = gl::GetUniformLocation(program_, "uFloor");
    const gl::GLint uMatte = gl::GetUniformLocation(program_, "uMatte");
    gl::Uniform1f(gl::GetUniformLocation(program_, "uFloorTile"), kParquetTile);
    gl::Uniform3f(gl::GetUniformLocation(program_, "uBackground"), kBackground[0], kBackground[1], kBackground[2]);
    // The floor lit around what is looked at, further the further back the camera is.
    const float reach = std::max(1.0f, std::sqrt((target.x - eye_.x) * (target.x - eye_.x) + (target.y - eye_.y) * (target.y - eye_.y) +
                                                 (target.z - eye_.z) * (target.z - eye_.z)) / 3.6f);
    gl::Uniform4f(gl::GetUniformLocation(program_, "uFloorFade"), target.x, target.z + 0.8f, 1.0f * reach, 6.5f * reach);
    auto drawPart = [&](const Part& p) {
        gl::UniformMatrix4fv(uModel, 1, gl::FALSE_, p.model.data());
        gl::Uniform3f(uColour, p.colour.x, p.colour.y, p.colour.z);
        gl::Uniform1i(uTexFaces, p.texFaces);
        gl::Uniform4f(uTexRect, p.texRect[0], p.texRect[1], p.texRect[2], p.texRect[3]);
        gl::Uniform1f(uGlow, p.glow);
        gl::Uniform1f(uShine, p.shine);
        gl::Uniform1i(uMatte, p.matte ? 1 : 0);
        if (p.texFaces) gl::BindTexture(gl::TEXTURE_2D, p.texture);
        if (p.hidden) return;
        if (p.custom >= 0) {
            const Custom& c = custom_[p.custom];
            if (p.label) {
                const float* area = p.labelWhole ? c.labelArea : c.pictureArea;
                gl::Uniform1i(uHasLabel, 1);
                gl::Uniform4f(uLabelArea, area[0], area[1], area[2], area[3]);
                gl::Uniform4f(uLabelRect, p.labelRect[0], p.labelRect[1], p.labelRect[2], p.labelRect[3]);
                gl::Uniform1i(uLabelTurn, c.labelTurn);
                gl::ActiveTexture(gl::TEXTURE0 + 2);
                gl::BindTexture(gl::TEXTURE_2D, p.label);
                gl::ActiveTexture(gl::TEXTURE0);
            }
            gl::BindVertexArray(c.vao);
            const int images = static_cast<int>(c.textures.size());
            for (const GltfModel::Batch& b : c.batches) {
                const bool textured = b.image >= 0 && b.image < images, bumped = b.normals >= 0 && b.normals < images;
                gl::Uniform1i(uTexFaces, textured ? 1 << kGltfFace : 0);
                if (textured) gl::BindTexture(gl::TEXTURE_2D, c.textures[static_cast<size_t>(b.image)]);
                gl::Uniform1i(uMask, textured && b.mask ? 1 : 0);
                if (p.label) gl::Uniform1i(uHasLabel, c.labelImage < 0 || c.labelImage == b.image ? 1 : 0);
                gl::Uniform1i(uHasNormals, bumped ? 1 : 0);
                if (bumped) {
                    gl::ActiveTexture(gl::TEXTURE0 + 1);
                    gl::BindTexture(gl::TEXTURE_2D, c.textures[static_cast<size_t>(b.normals)]);
                    gl::ActiveTexture(gl::TEXTURE0);
                }
                gl::DrawArrays(gl::TRIANGLES, b.first, b.count);
            }
            gl::Uniform1i(uHasNormals, 0);
            gl::Uniform1i(uHasLabel, 0);
            gl::Uniform1i(uMask, 0);
            gl::BindVertexArray(vao_);
        } else if (p.mesh >= 0) gl::DrawArrays(gl::TRIANGLES, meshFirst_[p.mesh], meshCount_[p.mesh]);
        else if (p.cylinder) gl::DrawArrays(gl::TRIANGLES, cylinderFirst_, cylinderCount_);
        else gl::DrawArrays(gl::TRIANGLES, cubeFirst_, cubeCount_);
    };
    // What the floor reflects, the floor over it, then the scene.
    gl::Uniform1i(uFloor, 0);
    gl::Uniform1i(uMirror, 1);
    for (const Part& p : parts_)
        if (p.reflect) drawPart(p);
    gl::Uniform1i(uMirror, 0);
    gl::Uniform1i(uFloor, 1);
    gl::Enable(gl::BLEND);
    gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
    Part floor;
    floor.model = translate(target.x, kFloorY - 0.005f, 0) * scale(120.0f, 0.01f, 120.0f);
    floor.colour = {0.1f, 0.1f, 0.115f};
    floor.texture = roomTextures_.get(kParquet);
    floor.texFaces = 1;
    drawPart(floor);
    gl::Disable(gl::BLEND);
    gl::Uniform1i(uFloor, 0);
    for (const Part& p : parts_) drawPart(p);

    gl::BindVertexArray(0);
    gl::Disable(gl::DEPTH_TEST);
    if (multisample_) {
        gl::BindFramebuffer(gl::READ_FRAMEBUFFER, msFbo_);
        gl::BindFramebuffer(gl::DRAW_FRAMEBUFFER, fbo_);
        gl::BlitFramebuffer(0, 0, width, height, 0, 0, width, height, gl::COLOR_BUFFER_BIT, gl::NEAREST);
    }
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    gl::BindTexture(gl::TEXTURE_2D, colour_);
    gl::GenerateMipmap(gl::TEXTURE_2D);
    gl::BindTexture(gl::TEXTURE_2D, 0);
    return colour_;
}

}  // namespace plumnin
