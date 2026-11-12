#pragma once

#include <array>
#include <vector>

#include "gl.h"
#include "renderer.h"

namespace plumnin {

// Draws the 2D picture bigger than the console made it with one of the experimental
// upscalers, in place of the plain quad. Its programs are built the first time each
// one is asked for, so a session that never turns it on pays nothing.
class Upscaler {
public:
    Upscaler() = default;
    ~Upscaler();
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    // Draws `source` (w x h texels, top row first) into `area`, which must be the current
    // viewport of the default framebuffer. False if the upscaler could
    // not be built; then nothing was drawn.
    bool draw(Upscale2D kind, gl::GLuint source, int w, int h, const Viewport& area);

private:
    // One doubling of Anime4K: its intermediate textures at the input's size, and the
    // result at twice that.
    struct Stage {
        int width = 0, height = 0;
        std::vector<gl::GLuint> textures;  // one per name in names_
        gl::GLuint output = 0;
    };

    bool buildXbr();
    bool buildAnime4K();
    void runAnime4K(gl::GLuint source, Stage& stage, int w, int h);

    gl::GLuint vao_ = 0;
    gl::GLuint xbr_ = 0;
    bool xbrTried_ = false;
    gl::GLuint display_ = 0;  // draws Anime4K's result on screen
    std::vector<gl::GLuint> passes_;
    std::vector<std::vector<gl::GLint>> samplers_;  // per pass, the location of each bound texture
    std::vector<const char*> names_;                // the intermediate textures Anime4K saves
    bool anime4kTried_ = false;
    gl::GLuint framebuffer_ = 0;
    std::array<Stage, 2> stages_{};
};

}  // namespace plumnin
