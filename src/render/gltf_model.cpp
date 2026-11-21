#include "gltf_model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "stb_image.h"

namespace plumnin {

namespace {

// One of a material's textures, decoded once per image.
int image(const cgltf_texture_view& view, const std::string& dir, GltfModel& out,
          std::map<const cgltf_image*, int>& seen) {
    if (!view.texture || !view.texture->image) return -1;
    const cgltf_image* img = view.texture->image;
    if (const auto it = seen.find(img); it != seen.end()) return it->second;
    std::vector<unsigned char> bytes;
    if (img->buffer_view) {
        const unsigned char* p = cgltf_buffer_view_data(img->buffer_view);
        if (p) bytes.assign(p, p + img->buffer_view->size);
    } else if (img->uri && std::string(img->uri).rfind("data:", 0) != 0) {
        std::string uri = img->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::char_traits<char>::length(uri.c_str()));
        std::ifstream f(dir + uri, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    int index = -1;
    int w = 0, h = 0, n = 0;
    if (unsigned char* rgba = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4)) {
        GltfModel::Image im;
        im.width = w;
        im.height = h;
        im.pixels.assign(reinterpret_cast<const uint32_t*>(rgba), reinterpret_cast<const uint32_t*>(rgba) + static_cast<size_t>(w) * h);
        stbi_image_free(rgba);
        index = static_cast<int>(out.images.size());
        out.images.push_back(std::move(im));
    }
    seen[img] = index;
    return index;
}

void transform(const float* m, const float* in, float* outp, bool point) {
    for (int r = 0; r < 3; ++r)
        outp[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + (point ? m[12 + r] : 0);
}

}  // namespace

bool loadGltf(const std::string& path, GltfModel& out, std::string* error) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        *error = "no es un modelo glTF";
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        *error = "faltan los datos del modelo";
        return false;
    }
    const std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    std::map<const cgltf_image*, int> images;
    out = GltfModel();
    bool first = true;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        float world[16];
        cgltf_node_transform_world(&node, world);
        // The normals turn with the node (a model scaled unevenly is rare enough to ignore).
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nor = nullptr, *uv = nullptr, *col = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nor = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
            }
            if (!pos) continue;
            float base[4] = {1, 1, 1, 1};
            int tex = -1, normals = -1;
            if (prim.material) normals = image(prim.material->normal_texture, dir, out, images);
            if (prim.material && prim.material->has_pbr_metallic_roughness) {
                std::copy(prim.material->pbr_metallic_roughness.base_color_factor,
                          prim.material->pbr_metallic_roughness.base_color_factor + 4, base);
                tex = image(prim.material->pbr_metallic_roughness.base_color_texture, dir, out, images);
            }
            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            GltfModel::Batch batch;
            batch.first = static_cast<int>(out.vertices.size() / 12);
            batch.image = tex;
            batch.normals = normals;
            batch.mask = prim.material && prim.material->alpha_mode == cgltf_alpha_mode_mask;
            for (cgltf_size i = 0; i < count; ++i) {
                const cgltf_size v = prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i;
                float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0}, c[4] = {1, 1, 1, 1}, wp[3], wn[3];
                cgltf_accessor_read_float(pos, v, p, 3);
                if (nor) cgltf_accessor_read_float(nor, v, n, 3);
                if (uv) cgltf_accessor_read_float(uv, v, t, 2);
                if (col) cgltf_accessor_read_float(col, v, c, 4);
                transform(world, p, wp, true);
                transform(world, n, wn, false);
                const float l = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                if (l > 0) wn[0] /= l, wn[1] /= l, wn[2] /= l;
                for (int k = 0; k < 3; ++k) {
                    if (first || wp[k] < out.min[k]) out.min[k] = wp[k];
                    if (first || wp[k] > out.max[k]) out.max[k] = wp[k];
                }
                first = false;
                out.vertices.insert(out.vertices.end(), {wp[0], wp[1], wp[2], wn[0], wn[1], wn[2], t[0], t[1],
                                                         static_cast<float>(kGltfFace), base[0] * c[0], base[1] * c[1],
                                                         base[2] * c[2]});
            }
            // Without normals in the file, each triangle's own.
            if (!nor) {
                for (size_t v = static_cast<size_t>(batch.first) * 12; v + 36 <= out.vertices.size(); v += 36) {
                    float* a = &out.vertices[v];
                    float* b = a + 12;
                    float* c = b + 12;
                    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (l > 0) n[0] /= l, n[1] /= l, n[2] /= l;
                    for (float* q : {a, b, c}) std::copy(n, n + 3, q + 3);
                }
            }
            batch.count = static_cast<int>(out.vertices.size() / 12) - batch.first;
            if (batch.count > 0) out.batches.push_back(batch);
        }
    }
    cgltf_free(data);
    if (out.batches.empty()) {
        *error = "el modelo no tiene triángulos";
        return false;
    }
    return true;
}

}  // namespace plumnin
