#include "profile3d.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "../core/ini_names.h"

namespace plumnin {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool yes(const std::string& v) {
    const std::string l = lower(v);
    return l == "1" || iniName(l) == "yes" || l == "true";
}

float percent(const std::string& v) { return std::clamp(std::atoi(v.c_str()), 0, 100) / 100.0f; }

float number(const std::string& v, float lo, float hi) {
    return std::clamp(static_cast<float>(std::atof(v.c_str())), lo, hi);
}

}  // namespace

void copy3D(const RenderOptions& from, RenderOptions& to) {
    to.pseudo3d = from.pseudo3d;
    to.style = from.style;
    to.depth = from.depth;
    to.tilt = from.tilt;
    to.yaw = from.yaw;
    to.zoom = from.zoom;
    to.extrusion = from.extrusion;
    to.thickness = from.thickness;
    to.shadows = from.shadows;
    to.tiltCamera = from.tiltCamera;
    to.lighting = from.lighting;
    to.liveCamera = from.liveCamera;
    to.highBInFront = from.highBInFront;
    to.flatB = from.flatB;
}

bool same3D(const RenderOptions& a, const RenderOptions& b) {
    return a.pseudo3d == b.pseudo3d && a.style == b.style && a.depth == b.depth && a.tilt == b.tilt &&
           a.yaw == b.yaw && a.zoom == b.zoom && a.extrusion == b.extrusion && a.thickness == b.thickness &&
           a.shadows == b.shadows &&
           a.tiltCamera == b.tiltCamera && a.lighting == b.lighting && a.liveCamera == b.liveCamera && a.highBInFront == b.highBInFront && a.flatB == b.flatB;
}

bool read3DKey(const std::string& key, const std::string& value, RenderOptions& r) {
    if (key == "pseudo_3d") r.pseudo3d = yes(value);
    else if (key == "style_3d") r.style = iniName(lower(value)) == "layers" ? Style3D::Layers : Style3D::Voxels;
    else if (key == "depth_3d") r.depth = percent(value);
    else if (key == "tilt_3d") r.tilt = number(value, kMinTilt, kMaxTilt);
    else if (key == "yaw_3d") r.yaw = number(value, -kMaxYaw, kMaxYaw);
    else if (key == "zoom_3d") r.zoom = number(value, kMinZoom, kMaxZoom);
    else if (key == "extrusion_3d") r.extrusion = percent(value);
    else if (key == "thickness_3d") r.thickness = percent(value);
    else if (key == "shadows_3d") r.shadows = yes(value);
    else if (key == "tilt_camera") r.tiltCamera = yes(value);
    else if (key == "lighting_3d") r.lighting = yes(value);
    else if (key == "live_camera") r.liveCamera = yes(value);
    else if (key == "high_b_in_front") r.highBInFront = yes(value);
    else if (key == "flat_b") r.flatB = yes(value);
    else return false;
    return true;
}

void write3D(std::ostream& out, const RenderOptions& r) {
    auto flag = [](bool b) { return b ? "yes" : "no"; };
    out << "pseudo_3d = " << flag(r.pseudo3d) << "\n"
        << "# style_3d: voxels or layers. depth_3d, thickness_3d and extrusion_3d go from 0 to 100.\n"
        << "# Camera: tilt_3d from -10 to 80 degrees, yaw_3d from -75 to 75, zoom_3d from 0.5 to 3.\n"
        << "style_3d = " << (r.style == Style3D::Layers ? "layers" : "voxels") << "\n"
        << "depth_3d = " << std::lround(r.depth * 100) << "\n"
        << "tilt_3d = " << std::lround(r.tilt) << "\n"
        << "yaw_3d = " << std::lround(r.yaw) << "\n"
        << "zoom_3d = " << std::lround(r.zoom * 100) / 100.0 << "\n"
        << "extrusion_3d = " << std::lround(r.extrusion * 100) << "\n"
        << "thickness_3d = " << std::lround(r.thickness * 100) << "\n"
        << "shadows_3d = " << flag(r.shadows) << "\n"
        << "# tilt_camera: do not look straight on (uses tilt_3d and yaw_3d). lighting_3d: side light\n"
        << "# and soft shadows from each plane. live_camera: the camera sways following the action.\n"
        << "tilt_camera = " << flag(r.tiltCamera) << "\n"
        << "lighting_3d = " << flag(r.lighting) << "\n"
        << "live_camera = " << flag(r.liveCamera) << "\n"
        << "# high_b_in_front: plane B's priority tiles go in front, with plane A's.\n"
        << "high_b_in_front = " << flag(r.highBInFront) << "\n"
        << "# flat_b: plane B stays flat, as part of the sky.\n"
        << "flat_b = " << flag(r.flatB) << "\n";
}

std::string formatProfile(const RenderOptions& r, const std::string& game) {
    std::ostringstream out;
    out << "# plumnin 3D profile for " << game << ". While this game is loaded, these settings\n"
        << "# replace the 3D ones in [video] in plumnin.ini.\n\n[profile_3d]\n";
    write3D(out, r);
    return out.str();
}

bool parseProfile(const std::string& text, RenderOptions& r) {
    std::istringstream in(text);
    std::string line, section;
    bool found = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = iniName(lower(trim(line.substr(1, line.size() - 2))));
            found |= section == "profile_3d";
            continue;
        }
        const size_t eq = line.find('=');
        if (section != "profile_3d" || eq == std::string::npos) continue;
        read3DKey(iniName(lower(trim(line.substr(0, eq)))), trim(line.substr(eq + 1)), r);
    }
    return found;
}

}  // namespace plumnin
