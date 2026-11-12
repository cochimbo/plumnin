#pragma once

#include <algorithm>

#include "../core/video_frame.h"

namespace plumnin {

enum class ScaleMode { Integer, Fit, Stretch };

// Camera limits for the pseudo-3D view, shared by the mouse, the pad and the menus.
constexpr float kMinTilt = -10.0f, kMaxTilt = 80.0f, kMaxYaw = 75.0f, kMinZoom = 0.5f, kMaxZoom = 3.0f;

// How the pseudo-3D view builds each plane: flat cut-outs, or one block per pixel.
enum class Style3D { Layers, Voxels };

// Look of the 2D picture: plain, with the dark gaps between a TV's lines, or a whole
// CRT (scanlines, a slightly curved screen, the phosphor mask and darker corners).
enum class Filter2D { None, Scanlines, Crt };

// Experimental: the 2D picture drawn bigger than the console made it, guessing the
// detail. xBR rounds the stairs of the diagonals off; Anime4K is a small neural
// network that doubles the picture once or twice. Either replaces the scanlines or CRT.
enum class Upscale2D { None, Xbr, Anime4K };

// Display options chosen in the video menu.
struct RenderOptions {
    ScaleMode scale = ScaleMode::Integer;
    bool smooth = false;     // bilinear filtering instead of sharp pixels
    Filter2D filter = Filter2D::None;  // only the 2D view uses it
    float filterStrength = 0.6f;       // 0..1, how strong the scanlines and the rest are
    Upscale2D upscale = Upscale2D::None;  // only the 2D view uses it; wins over the filter
    bool pseudo3d = false;   // toggled by the "efecto 3D" hotkey: draw with the pseudo-3D renderer
    // Pseudo-3D look, each 0..1 except the tilt.
    Style3D style = Style3D::Voxels;
    float depth = 0.6f;      // how far apart the layers are
    bool tiltCamera = true;  // off: the camera looks straight on, whatever tilt and yaw say
    float tilt = 25.0f;      // camera tilt in degrees, -10..80; at 0 the picture looks flat
    float yaw = 0.0f;        // camera turn around the vertical axis in degrees, -75..75
    float zoom = 1.0f;       // 0.5..3
    float extrusion = 0.5f;  // voxels: how far solid background tiles reach back
    float thickness = 1.0f;  // how much of the gap below a plane its sides (or blocks) fill
    bool shadows = true;     // sprites cast a shadow on the planes behind them
    bool lighting = true;    // side light on the blocks and soft shadows of every plane on those behind
    bool liveCamera = false; // the camera sways a few degrees following the action
    bool squarePixels = false;  // show pixels square instead of the TV's 4:3 picture
    bool highBInFront = false;  // high-priority plane B tiles stand with the priority plane
    bool flatB = false;         // plane B stays flat, as part of the sky
};

// Width on screen of one pixel, for a line height of 1. The TV shows 224 lines at 4:3
// whatever the horizontal resolution (PAL's 240 lines just take more of the screen);
// the widescreen columns are as wide as the console's own. A handheld's own screen (the
// Game Boy's) has its own shape instead: frame.screenAspect.
inline float pixelAspect(const VideoFrame& frame, bool squarePixels) {
    const float aspect = frame.screenAspect > 0 ? frame.screenAspect : 4.0f / 3.0f;
    return squarePixels ? 1.0f
                        : static_cast<float>(frame.aspectLines) * aspect / static_cast<float>(frame.consoleWidth());
}

// Where to draw, in OpenGL window coordinates (origin at the bottom left).
struct Viewport {
    int x = 0, y = 0, width = 0, height = 0;
};

// Where the 2D views draw the picture inside `area`: as large as the scale mode allows,
// centred, the lines kept sharp with the integer scale.
inline Viewport pictureArea(const VideoFrame& frame, const Viewport& area, const RenderOptions& options) {
    // Size of the picture in "screen pixels": lines are one unit high (two interlaced
    // lines share one), and pixels are as wide as the TV makes them.
    const float lines = frame.interlaced ? frame.height / 2.0f : static_cast<float>(frame.height);
    const float pictureW = static_cast<float>(frame.width) * pixelAspect(frame, options.squarePixels);
    int w = area.width, h = area.height;
    if (options.scale == ScaleMode::Integer) {
        // Largest integer scale of the lines that fits, centred, to keep them sharp.
        const int scale = std::max(1, std::min(static_cast<int>(area.width / pictureW), static_cast<int>(area.height / lines)));
        w = static_cast<int>(pictureW * static_cast<float>(scale) + 0.5f);
        h = static_cast<int>(lines) * scale;
    } else if (options.scale == ScaleMode::Fit) {
        if (static_cast<float>(area.width) * lines > static_cast<float>(area.height) * pictureW)
            w = static_cast<int>(static_cast<float>(area.height) * pictureW / lines);
        else
            h = static_cast<int>(static_cast<float>(area.width) * lines / pictureW);
    }
    return Viewport{area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h};
}

// A renderer turns a VideoFrame into pixels on screen. The emulator core never calls
// into a renderer; the frontend hands it each finished frame. A pseudo-3D renderer
// is just another implementation of this interface. Renderers draw with OpenGL 3.3 into
// the current context.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual const char* name() const = 0;
    virtual void draw(const VideoFrame& frame, const Viewport& area) = 0;
    void setOptions(const RenderOptions& options) { options_ = options; }

protected:
    RenderOptions options_;
};

}  // namespace plumnin
