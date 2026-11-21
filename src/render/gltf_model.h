#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumnin {

// A 3D model read from a glTF 2.0 file (.glb, or .gltf with its files beside it), for the
// library's 3D boxes: its triangles in the shelf's vertex layout (kConsoleVertexFloats
// each, the colour being the material's base colour) and its materials' textures.
struct GltfModel {
    std::vector<float> vertices;
    // Runs of triangles drawn with one texture and one normal map (indices into images, -1
    // for none).
    struct Batch {
        int first = 0, count = 0;
        int image = -1, normals = -1;
        bool mask = false;  // its texture's see-through parts are holes (alphaMode MASK)
    };
    std::vector<Batch> batches;
    struct Image {
        std::vector<uint32_t> pixels;  // RGBA in memory order
        int width = 0, height = 0;
    };
    std::vector<Image> images;
    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};  // its bounds, as the file has it
};

// The face number its vertices carry: the shelf shows a texture on them when the batch has one.
constexpr int kGltfFace = 6;

// Reads every mesh of the file's scene, placed by its nodes. False, with the reason in
// *error, if it cannot.
bool loadGltf(const std::string& path, GltfModel& out, std::string* error);

}  // namespace plumnin
