#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../core/video_frame.h"

namespace plumnin {

// The planes the pseudo-3D view stands apart, back to front.
enum Plane { PlaneSky, PlaneB, PlaneA, PlaneSprites, PlanePriority, kPlanes };

// One picture per plane (0xAARRGGBB, alpha 0 where a plane is empty), all the same size.
struct Planes {
    int width = 0, height = 0;
    std::array<std::vector<uint32_t>, kPlanes> layer;
    std::vector<uint32_t>& operator[](int p) { return layer[p]; }
    const std::vector<uint32_t>& operator[](int p) const { return layer[p]; }
};

struct SplitOptions {
    bool highBInFront = false;  // high-priority plane B tiles go to the Priority plane
    bool flatB = false;         // plane B is drawn flat, as part of the sky
};

// Splits the frame into planes. In interlace only the even field is used, so the
// planes keep the normal height. The sky is always full: the backdrop (plus all of
// plane B with flatB), with the gaps behind plane B filled from the nearest sky pixel
// on the line. Plane A takes the low-priority tiles of plane A and the window, Priority
// the high-priority ones, and Sprites every sprite.
//
// The hardware's priority order does not match the planes' depth order exactly (a
// sprite behind the background, as the NES and the Game Boy allow, or a high priority
// plane B tile covering low priority sprites and plane A). So, for every pixel, the plane holding what the
// console actually shows gets its final colour, and the planes in front of it lose
// that pixel; the planes behind keep theirs, so tilting the camera shows what is
// hidden. With the camera straight on, the 3D view is exactly the 2D picture.
void splitPlanes(const VideoFrame& frame, const SplitOptions& options, Planes& planes);

// Writes into the alpha of every solid pixel how far it is from the plane's edge, in
// pixels (city-block distance to the nearest empty pixel or the picture border), capped
// at kMaxEdgeDistance: alpha = 255 - 16 * (kMaxEdgeDistance - distance). Empty pixels
// keep alpha 0, so anything testing alpha >= 128 still sees the same shape. The voxel
// view uses it to round off the edges of every shape.
constexpr int kMaxEdgeDistance = 4;
void markEdgeDistance(std::vector<uint32_t>& plane, int width, int height);

// Marks the pixels of plane A (0xFFFFFFFF or 0, planes' size) that belong to solid
// tiles: tile patterns of which at most a quarter of the pixels on screen are
// transparent. Ground, walls and platforms are usually solid; foliage, clouds and
// outlines are not. The voxel view extrudes solid tiles far back, like 3DNES does.
void markSolidTiles(const VideoFrame& frame, const Planes& planes, std::vector<uint32_t>& solid);

// Clears plane B wherever plane A has a solid tile. Extruded solid tiles reach back past
// plane B, so whatever plane B has there would sit inside the block; and since every
// plane is scaled to line up with the 2D picture as seen straight on, plane B is a
// little larger than plane A and those hidden pixels would poke out of the block's
// sides when the camera tilts (water showing through the ground in front of it). With
// the camera straight on nothing changes: plane A covers those pixels.
void clearBehindSolid(const std::vector<uint32_t>& solid, Planes& planes);

// The "live camera": a small sway of the pseudo-3D camera that follows the action, so
// the depth shows through parallax even when the camera looks straight on. It leans
// towards where the sprites are on screen and towards where the level scrolls (most
// games keep the player centred and move the background instead), at most kSwayYaw
// degrees sideways and kSwayTilt up or down, and eases towards that so it never jumps.
class LiveCamera {
public:
    static constexpr float kSwayYaw = 4.0f, kSwayTilt = 3.0f;
    // Feed it every frame drawn; a frame already seen (pause, slow motion) only eases.
    void update(const VideoFrame& frame, const Planes& planes);
    void reset() { *this = LiveCamera(); }
    float yaw() const { return yaw_; }    // degrees to add to the camera's turn
    float tilt() const { return tilt_; }  // degrees to add to its tilt

private:
    float yaw_ = 0, tilt_ = 0;
    float targetYaw_ = 0, targetTilt_ = 0;
    float speed_ = 0;  // horizontal scroll in pixels per frame, smoothed
    int lastScroll_ = 0;
    uint64_t lastFrame_ = 0;
    bool seen_ = false;
};

}  // namespace plumnin
