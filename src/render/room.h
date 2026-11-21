#pragma once

#include <array>
#include <vector>

namespace plumnin {

// The living room of the late 80s around the 3D library's shelf: papered walls over a
// wooden dado, a parquet floor with a rug, a window with its curtains, a floor lamp, a
// poster, and a television on a cabinet with a video recorder in it. All of it is made
// here from boxes, cylinders and textures drawn in code, but the television, which is a
// glTF model (television.glb) when there is one.
//
// It grows with the shelf: a few metres of wall on each side and some above it. Units are
// the shelf's: a Mega Drive case is 1 wide (13.5 cm).
struct RoomPiece {
    // A unit cube (or a cylinder of diameter 1 along z), placed by `model` (column-major).
    std::array<float, 16> model{};
    std::array<float, 3> colour{1, 1, 1};
    int texture = -1;        // a RoomTexture, shown on the faces in texFaces
    int texFaces = 0;        // bit per face (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z)
    std::array<float, 4> texRect{0, 0, 1, 1};
    bool cylinder = false;
    bool television = false;  // the television's model, `model` standing it on the floor there
    float glow = 0;
    float shine = 0.1f;
    bool reflect = true;     // seen in the floor
};

enum RoomTexture { kWallpaper, kPanelling, kParquet, kRug, kPoster, kWindowView, kCurtain, kVcrFront, kRoomTextures };

// The room's textures, drawn once when they are first needed (an OpenGL context current).
class RoomTextures {
public:
    RoomTextures() = default;
    ~RoomTextures();
    RoomTextures(const RoomTextures&) = delete;
    RoomTextures& operator=(const RoomTextures&) = delete;
    unsigned int get(int which);

private:
    std::array<unsigned int, kRoomTextures> ids_{};
};

// How far the parquet's pattern repeats, in the shelf's units.
constexpr float kParquetTile = 1.2f / 0.135f;
// The television's real width.
constexpr float kTelevisionWidth = 0.6f / 0.135f;

// The room around a shelf `width` wide and `height` tall standing on the floor at
// `floorY`, its back against the wall at `backZ`.
void buildRoom(float width, float height, float floorY, float backZ, std::vector<RoomPiece>& out);

}  // namespace plumnin
