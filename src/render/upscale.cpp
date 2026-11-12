#include "upscale.h"

#include <cstring>
#include <string>

#include "anime4k_shaders.h"

namespace plumnin {

namespace {

// A unit quad from gl_VertexID, as in gl_renderers.cpp. The screen one turns the
// picture right side up; the offscreen one keeps the texture's rows as they are, so
// passes that feed each other agree on which way is up.
constexpr const char* kScreenVertex = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vUv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kOffscreenVertex = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kDisplayFragment = R"(#version 330 core
uniform sampler2D uImage;
in vec2 vUv;
out vec4 outColor;
void main() { outColor = vec4(clamp(texture(uImage, vUv).rgb, 0.0, 1.0), 1.0); }
)";

// xBR level 2, after Hyllian's xbr-lv2 shader (MIT). Each screen pixel falls in one
// corner of a console pixel E; around it, the 5x5 neighbourhood (minus its corners)
// says whether an edge crosses that corner and at what slope (45, 30 or 60 degrees),
// by comparing how much the brightness changes along each direction. When one does,
// the corner takes the colour of the neighbour across the edge, softened over about
// one screen pixel. The four corners are worked out at once, one per vector component.
constexpr const char* kXbrFragment = R"(#version 330 core
uniform sampler2D uImage;
uniform vec2 uSource;  // texture size in texels
uniform vec2 uOutput;  // viewport size in screen pixels
in vec2 vUv;
out vec4 outColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722) * 48.0;

vec3 at(vec2 uv) { return texture(uImage, uv).rgb; }
vec4 luma(vec3 a, vec3 b, vec3 c, vec3 d) { return vec4(dot(a, kLuma), dot(b, kLuma), dot(c, kLuma), dot(d, kLuma)); }
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
vec4 eq(vec4 a, vec4 b) { return vec4(lessThan(df(a, b), vec4(15.0))); }
vec4 neq(vec4 a, vec4 b) { return 1.0 - eq(a, b); }
vec4 differ(vec4 a, vec4 b) { return vec4(notEqual(a, b)); }
float cdf(vec3 a, vec3 b) { vec3 d = abs(a - b); return d.r + d.g + d.b; }
vec4 distance8(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}

void main() {
    vec2 pos = vUv * uSource;
    vec2 fp = fract(pos);
    vec2 tc = (floor(pos) + 0.5) / uSource;
    vec2 dx = vec2(1.0 / uSource.x, 0.0), dy = vec2(0.0, 1.0 / uSource.y);

    //     A1 B1 C1
    //  A0  A  B  C C4
    //  D0  D  E  F F4
    //  G0  G  H  I I4
    //     G5 H5 I5
    vec3 A1 = at(tc - dx - 2.0 * dy), B1 = at(tc - 2.0 * dy), C1 = at(tc + dx - 2.0 * dy);
    vec3 A = at(tc - dx - dy), B = at(tc - dy), C = at(tc + dx - dy);
    vec3 D = at(tc - dx), E = at(tc), F = at(tc + dx);
    vec3 G = at(tc - dx + dy), H = at(tc + dy), I = at(tc + dx + dy);
    vec3 G5 = at(tc - dx + 2.0 * dy), H5 = at(tc + 2.0 * dy), I5 = at(tc + dx + 2.0 * dy);
    vec3 A0 = at(tc - 2.0 * dx - dy), D0 = at(tc - 2.0 * dx), G0 = at(tc - 2.0 * dx + dy);
    vec3 C4 = at(tc + 2.0 * dx - dy), F4 = at(tc + 2.0 * dx), I4 = at(tc + 2.0 * dx + dy);

    // Component x looks at the bottom right corner; y, z and w at the corners met
    // turning a quarter at a time (top right, top left, bottom left).
    vec4 b = luma(B, D, H, F), c = luma(C, A, G, I);
    vec4 e = vec4(dot(E, kLuma));
    vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    vec4 i4 = luma(I4, C1, A0, G5), i5 = luma(I5, C4, A1, G0), h5 = luma(H5, F4, B1, D0);
    vec4 f4 = h5.yzwx;

    // The lines past which a corner belongs to the neighbour: 45, 30 and 60 degrees.
    const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0), Bo = vec4(1.0, 1.0, -1.0, -1.0), Co = vec4(1.5, 0.5, -0.5, 0.5);
    const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0), Cx = vec4(1.0, 1.0, -0.5, 0.0);
    const vec4 By = vec4(2.0, 0.5, -2.0, -0.5), Cy = vec4(2.0, 0.0, -1.0, 0.5);
    vec4 fx = Ao * fp.y + Bo * fp.x;
    vec4 fxLeft = Ao * fp.y + Bx * fp.x;
    vec4 fxUp = Ao * fp.y + By * fp.x;
    vec4 delta = vec4(clamp(uSource.y / uOutput.y, 0.05, 0.5));

    vec4 corner = differ(e, f) * differ(e, h) *
                  max(max(neq(f, b) * neq(h, d), eq(e, i) * neq(f, i4) * neq(h, i5)), max(eq(e, g), eq(e, c)));
    vec4 edge = vec4(lessThan(distance8(e, c, g, i, h5, f4, h, f), distance8(h, d, i5, f, i4, b, e, i))) * corner;
    vec4 edgeLeft = vec4(lessThanEqual(2.0 * df(f, g), df(h, c))) * differ(e, g) * differ(d, g) * edge;
    vec4 edgeUp = vec4(greaterThanEqual(df(f, g), 2.0 * df(h, c))) * differ(e, c) * differ(b, c) * edge;

    vec4 fx45 = edge * smoothstep(Co - delta, Co + delta, fx);
    vec4 fx30 = edgeLeft * smoothstep(Cx - delta, Cx + delta, fxLeft);
    vec4 fx60 = edgeUp * smoothstep(Cy - delta, Cy + delta, fxUp);
    vec4 nearer = vec4(lessThanEqual(df(e, f), df(e, h)));
    vec4 amount = max(max(fx30, fx60), fx45);

    vec3 res1 = mix(E, mix(H, F, nearer.x), amount.x);
    res1 = mix(res1, mix(B, D, nearer.z), amount.z);
    vec3 res2 = mix(E, mix(F, B, nearer.y), amount.y);
    res2 = mix(res2, mix(D, H, nearer.w), amount.w);
    outColor = vec4(mix(res1, res2, step(cdf(E, res1), cdf(E, res2))), 1.0);
}
)";

// What the mpv shaders expect around each pass: every bound texture under its own
// name, and the NAME_pos / NAME_tex / NAME_texOff helpers mpv defines for it.
std::string anime4kSource(const anime4k::Pass& pass) {
    std::string s = "#version 330 core\nin vec2 vUv;\nout vec4 outColor;\n";
    for (const char* name : pass.binds) {
        if (!name) break;
        const std::string n = name;
        s += "uniform sampler2D " + n + ";\n";
        s += "#define " + n + "_pos vUv\n";
        s += "#define " + n + "_size vec2(textureSize(" + n + ", 0))\n";
        s += "#define " + n + "_pt (1.0 / " + n + "_size)\n";
        s += "#define " + n + "_tex(p) texture(" + n + ", p)\n";
        s += "#define " + n + "_texOff(off) texture(" + n + ", vUv + " + n + "_pt * (off))\n";
    }
    s += pass.code;
    s += "void main() { outColor = hook(); }\n";
    return s;
}

gl::GLuint makeTarget(int w, int h, bool linear) {
    gl::GLuint t = 0;
    gl::GenTextures(1, &t);
    gl::BindTexture(gl::TEXTURE_2D, t);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA16F, w, h, 0, gl::RGBA, gl::HALF_FLOAT, nullptr);
    const gl::GLint filter = linear ? gl::LINEAR : gl::NEAREST;
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, filter);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, filter);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    return t;
}

void deleteStage(std::vector<gl::GLuint>& textures, gl::GLuint& output) {
    if (!textures.empty()) gl::DeleteTextures(static_cast<gl::GLsizei>(textures.size()), textures.data());
    textures.clear();
    if (output) gl::DeleteTextures(1, &output);
    output = 0;
}

}  // namespace

Upscaler::~Upscaler() {
    for (Stage& s : stages_) deleteStage(s.textures, s.output);
    for (gl::GLuint p : passes_) gl::DeleteProgram(p);
    if (xbr_) gl::DeleteProgram(xbr_);
    if (display_) gl::DeleteProgram(display_);
    if (framebuffer_) gl::DeleteFramebuffers(1, &framebuffer_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
}

bool Upscaler::buildXbr() {
    if (!xbrTried_) {
        xbrTried_ = true;
        xbr_ = gl::buildProgram(kScreenVertex, kXbrFragment);
    }
    return xbr_ != 0;
}

bool Upscaler::buildAnime4K() {
    if (anime4kTried_) return display_ != 0;
    anime4kTried_ = true;
    for (const anime4k::Pass& pass : anime4k::kPasses) {
        const gl::GLuint program = gl::buildProgram(kOffscreenVertex, anime4kSource(pass).c_str());
        passes_.push_back(program);
        if (!program) return false;
        std::vector<gl::GLint> locations;
        for (const char* name : pass.binds) {
            if (!name) break;
            locations.push_back(gl::GetUniformLocation(program, name));
            bool known = std::strcmp(name, "MAIN") == 0;
            for (const char* n : names_) known |= std::strcmp(n, name) == 0;
            if (!known) names_.push_back(name);
        }
        samplers_.push_back(std::move(locations));
    }
    gl::GenFramebuffers(1, &framebuffer_);
    display_ = gl::buildProgram(kScreenVertex, kDisplayFragment);
    return display_ != 0;
}

// One doubling: every pass of the network in turn, each into its texture, the last
// into the stage's output.
void Upscaler::runAnime4K(gl::GLuint source, Stage& stage, int w, int h) {
    if (stage.width != w || stage.height != h) {
        deleteStage(stage.textures, stage.output);
        for (size_t i = 0; i < names_.size(); ++i) stage.textures.push_back(makeTarget(w, h, false));
        stage.output = makeTarget(w * 2, h * 2, true);
        stage.width = w;
        stage.height = h;
    }
    auto texture = [&](const char* name) -> gl::GLuint {
        if (std::strcmp(name, "MAIN") == 0) return source;
        for (size_t i = 0; i < names_.size(); ++i)
            if (std::strcmp(names_[i], name) == 0) return stage.textures[i];
        return 0;
    };
    gl::BindFramebuffer(gl::FRAMEBUFFER, framebuffer_);
    for (size_t p = 0; p < passes_.size(); ++p) {
        const anime4k::Pass& pass = anime4k::kPasses[p];
        const gl::GLuint target = std::strcmp(pass.save, "MAIN") == 0 ? stage.output : texture(pass.save);
        gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, target, 0);
        gl::Viewport(0, 0, pass.doubles ? w * 2 : w, pass.doubles ? h * 2 : h);
        gl::UseProgram(passes_[p]);
        for (size_t b = 0; b < samplers_[p].size(); ++b) {
            gl::ActiveTexture(gl::TEXTURE0 + static_cast<gl::GLenum>(b));
            gl::BindTexture(gl::TEXTURE_2D, texture(pass.binds[b]));
            gl::Uniform1i(samplers_[p][b], static_cast<gl::GLint>(b));
        }
        gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
    }
    gl::ActiveTexture(gl::TEXTURE0);
}

bool Upscaler::draw(Upscale2D kind, gl::GLuint source, int w, int h, const Viewport& area) {
    if (!vao_) gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    if (kind == Upscale2D::Xbr) {
        if (!buildXbr()) return false;
        gl::UseProgram(xbr_);
        gl::Uniform1i(gl::GetUniformLocation(xbr_, "uImage"), 0);
        gl::Uniform2f(gl::GetUniformLocation(xbr_, "uSource"), static_cast<float>(w), static_cast<float>(h));
        gl::Uniform2f(gl::GetUniformLocation(xbr_, "uOutput"), static_cast<float>(area.width),
                      static_cast<float>(area.height));
        gl::ActiveTexture(gl::TEXTURE0);
        gl::BindTexture(gl::TEXTURE_2D, source);
        gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
        return true;
    }
    if (kind != Upscale2D::Anime4K || !buildAnime4K()) return false;
    // The network's first pass reads the picture between texels.
    gl::BindTexture(gl::TEXTURE_2D, source);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    // Doubles while the screen is more than 1.2 times bigger, as mpv does: once in a
    // small window, twice from about 540 lines up.
    gl::GLuint picture = source;
    int pw = w, ph = h;
    for (Stage& stage : stages_) {
        if (area.width <= pw * 6 / 5 || area.height <= ph * 6 / 5) break;
        runAnime4K(picture, stage, pw, ph);
        picture = stage.output;
        pw *= 2;
        ph *= 2;
    }
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    gl::Viewport(area.x, area.y, area.width, area.height);
    gl::UseProgram(display_);
    gl::Uniform1i(gl::GetUniformLocation(display_, "uImage"), 0);
    gl::ActiveTexture(gl::TEXTURE0);
    gl::BindTexture(gl::TEXTURE_2D, picture);
    gl::DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
    return true;
}

}  // namespace plumnin
