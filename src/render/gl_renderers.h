#pragma once

#include <array>
#include <vector>

#include "gl.h"
#include "planes.h"
#include "renderer.h"
#include "upscale.h"

namespace plumnin {

// Base for renderers that build a flat image of the frame's size and scale it to the
// window, at 4:3 unless square pixels are asked for.
class GlTextureRenderer : public Renderer {
public:
    GlTextureRenderer();
    ~GlTextureRenderer() override;
    GlTextureRenderer(const GlTextureRenderer&) = delete;
    GlTextureRenderer& operator=(const GlTextureRenderer&) = delete;
    void draw(const VideoFrame& frame, const Viewport& area) override;

protected:
    // Fills `out` (frame.width x frame.height, 0xAARRGGBB) from the frame.
    virtual void compose(const VideoFrame& frame, uint32_t* out) = 0;
    bool filterable_ = false;  // draws through the scanlines / CRT filter or the upscaler when one is chosen

private:
    gl::GLuint program_ = 0;
    gl::GLuint crtProgram_ = 0;
    gl::GLuint vao_ = 0;
    gl::GLuint texture_ = 0;
    int texWidth_ = 0, texHeight_ = 0;
    std::vector<uint32_t> pixels_;
    Upscaler upscaler_;
};

// The console's own picture.
class FlatRenderer : public GlTextureRenderer {
public:
    FlatRenderer() { filterable_ = true; }
    const char* name() const override { return "2D"; }

protected:
    void compose(const VideoFrame& frame, uint32_t* out) override;
};

// Debug view that tints each pixel by the layer it came from, which is the
// information the pseudo-3D renderer uses to separate depths.
class LayerRenderer : public GlTextureRenderer {
public:
    const char* name() const override { return "capas (depuración)"; }

protected:
    void compose(const VideoFrame& frame, uint32_t* out) override;
};

// Pulls the picture apart into planes and stands them up like a diorama seen from a
// tilted camera: the sky at the back, then plane B, plane A, the sprites, and the
// high-priority tiles at the front. In the layers style each raised plane is a cut-out
// whose sides reach down towards the plane below it; in the voxels style every pixel
// of a raised plane is a lit block. Sprites can cast a shadow on the background, and with
// lighting every plane casts a soft one on those behind it. The live camera sways the
// view a little to follow the action.
class Pseudo3DRenderer : public Renderer {
public:
    Pseudo3DRenderer();
    ~Pseudo3DRenderer() override;
    Pseudo3DRenderer(const Pseudo3DRenderer&) = delete;
    Pseudo3DRenderer& operator=(const Pseudo3DRenderer&) = delete;
    const char* name() const override { return "3D"; }
    void draw(const VideoFrame& frame, const Viewport& area) override;

private:
    gl::GLuint program_ = 0;
    gl::GLuint voxelProgram_ = 0;
    gl::GLuint vao_ = 0;
    std::array<gl::GLuint, kPlanes> textures_{};
    gl::GLuint solidTexture_ = 0;
    int texWidth_ = 0, texHeight_ = 0;
    Planes planes_;
    std::vector<uint32_t> solid_;  // markSolidTiles() for the voxel style
    LiveCamera live_;
};

}  // namespace plumnin
