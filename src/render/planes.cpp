#include "planes.h"

#include <algorithm>

namespace plumnin {

namespace {

// Source row of plane line y: in interlace mode 2 only the even field.
int sourceRow(const VideoFrame& frame, int y) { return frame.interlaced ? y * 2 : y; }

}  // namespace

void splitPlanes(const VideoFrame& frame, const SplitOptions& options, Planes& planes) {
    const int w = frame.width;
    const int h = frame.interlaced ? frame.height / 2 : frame.height;
    planes.width = w;
    planes.height = h;
    for (auto& p : planes.layer) p.assign(static_cast<size_t>(w) * h, 0);

    auto planeOf = [&](const Pixel& px) -> int {
        switch (px.layer) {
            case Layer::Backdrop: return PlaneSky;
            case Layer::Back:
                if (options.flatB) return PlaneSky;
                return px.priority && options.highBInFront ? PlanePriority : PlaneB;
            case Layer::Front:
            case Layer::Window: return px.priority ? PlanePriority : PlaneA;
            case Layer::Sprite: return PlaneSprites;
        }
        return PlaneSky;
    };

    for (int y = 0; y < h; ++y) {
        const int row = sourceRow(frame, y);
        uint32_t* sky = &planes[PlaneSky][static_cast<size_t>(y) * w];
        int firstSky = -1;
        for (int x = 0; x < w; ++x) {
            const int i = row * VideoFrame::kMaxWidth + x;
            const size_t o = static_cast<size_t>(y) * w + x;
            // Every layer into its plane.
            uint32_t b = frame.planeB[i];
            if (b >> 24) {
                planes[planeOf(frame.planeBInfo[i])][o] = b;
            } else {
                b = 0;
            }
            if (frame.planeA[i] >> 24) planes[planeOf(frame.planeAInfo[i])][o] = frame.planeA[i];
            if (frame.sprites[i] >> 24) planes[PlaneSprites][o] = frame.sprites[i];
            // What the console shows decides which plane is the visible one here.
            const int shown = planeOf(frame.info[i]);
            for (int p = shown + 1; p < kPlanes; ++p) planes[p][o] = 0;
            if (shown != PlaneSky) planes[shown][o] = frame.rgba[i] | 0xFF000000u;
            const bool bOpaque = (b >> 24) != 0;
            // Behind the planes: the backdrop.
            const uint32_t under = frame.backdrop[row];
            if (shown == PlaneSky) sky[x] = frame.rgba[i] | 0xFF000000u;
            else if (options.flatB) sky[x] = (bOpaque ? b : under) | 0xFF000000u;
            else if (!bOpaque) sky[x] = under | 0xFF000000u;  // under plane A or a sprite
            if (sky[x] != 0 && firstSky < 0) firstSky = x;
        }
        // Fill the sky behind plane B from the nearest sky pixel to the left (the first
        // one for the start of the line). A line with no sky at all keeps plane B's own
        // colours (or the backdrop), so tilting never shows a hole.
        if (firstSky < 0) {
            for (int x = 0; x < w; ++x) {
                const int i = row * VideoFrame::kMaxWidth + x;
                sky[x] = (frame.planeB[i] >> 24 ? frame.planeB[i] : frame.backdrop[row]) | 0xFF000000u;
            }
            continue;
        }
        uint32_t fill = sky[firstSky];
        for (int x = 0; x < w; ++x) {
            if (sky[x]) fill = sky[x];
            else sky[x] = fill;
        }
    }
}

void markEdgeDistance(std::vector<uint32_t>& plane, int w, int h) {
    // Two-pass chamfer distance: forward from the top left, then back from the bottom right.
    std::vector<uint8_t> d(plane.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!(plane[i] >> 24)) continue;
            const int up = y > 0 ? d[i - w] : 0;
            const int left = x > 0 ? d[i - 1] : 0;
            d[i] = static_cast<uint8_t>(std::min({up, left, kMaxEdgeDistance - 1}) + 1);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const int i = y * w + x;
            if (!d[i]) continue;
            const int down = y < h - 1 ? d[i + w] : 0;
            const int right = x < w - 1 ? d[i + 1] : 0;
            d[i] = static_cast<uint8_t>(std::min<int>({d[i], down + 1, right + 1}));
            plane[i] = (plane[i] & 0x00FFFFFFu) | static_cast<uint32_t>(255 - 16 * (kMaxEdgeDistance - d[i])) << 24;
        }
}

void markSolidTiles(const VideoFrame& frame, const Planes& planes, std::vector<uint32_t>& solid) {
    const int w = planes.width, h = planes.height;
    std::array<uint16_t, 2048> total{}, clear{};  // per tile pattern
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Pixel& a = frame.planeAInfo[sourceRow(frame, y) * VideoFrame::kMaxWidth + x];
            if (a.layer != Layer::Front && a.layer != Layer::Window) continue;
            const int pattern = a.source & 2047;
            ++total[pattern];
            if ((a.color & 0x0F) == 0) ++clear[pattern];
        }
    solid.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Pixel& a = frame.planeAInfo[sourceRow(frame, y) * VideoFrame::kMaxWidth + x];
            if ((a.layer != Layer::Front && a.layer != Layer::Window) || (a.color & 0x0F) == 0) continue;
            const int pattern = a.source & 2047;
            if (clear[pattern] * 4 <= total[pattern] && planes[PlaneA][static_cast<size_t>(y) * w + x])
                solid[static_cast<size_t>(y) * w + x] = 0xFFFFFFFFu;
        }
}

void clearBehindSolid(const std::vector<uint32_t>& solid, Planes& planes) {
    auto& b = planes[PlaneB];
    for (size_t i = 0; i < b.size() && i < solid.size(); ++i)
        if (solid[i]) b[i] = 0;
}

void LiveCamera::update(const VideoFrame& frame, const Planes& planes) {
    if (!seen_ || frame.frameNumber != lastFrame_) {
        // Where the sprites are, -1..1 from the middle of the picture.
        const auto& sprites = planes[PlaneSprites];
        double sx = 0, sy = 0;
        long count = 0;
        for (int y = 0; y < planes.height; y += 2)
            for (int x = 0; x < planes.width; x += 2)
                if ((sprites[static_cast<size_t>(y) * planes.width + x] >> 24) >= 128) {
                    sx += x;
                    sy += y;
                    ++count;
                }
        float cx = 0, cy = 0;
        if (count > 0) {
            cx = static_cast<float>(sx / count / planes.width * 2.0 - 1.0);
            cy = static_cast<float>(sy / count / planes.height * 2.0 - 1.0);
        }
        // How fast plane A scrolls sideways, on the middle line. The scroll value is 10
        // bits and grows to the right on screen, so the level moving left (the player
        // running right) makes it smaller.
        const int line = planes.height / 2;  // lines are counted without interlacing
        const int scroll = frame.lines[static_cast<size_t>(std::clamp(line, 0, VideoFrame::kMaxHeight - 1))].hscrollA;
        if (seen_ && frame.frameNumber == lastFrame_ + 1) {
            const int delta = ((lastScroll_ - scroll + 512) & 1023) - 512;
            speed_ += 0.15f * (static_cast<float>(delta) - speed_);
        }
        lastScroll_ = scroll;
        lastFrame_ = frame.frameNumber;
        seen_ = true;
        const float lean = std::clamp(0.6f * cx + 0.4f * speed_ / 4.0f, -1.0f, 1.0f);
        targetYaw_ = kSwayYaw * lean;
        targetTilt_ = kSwayTilt * std::clamp(cy, -1.0f, 1.0f);
    }
    yaw_ += 0.04f * (targetYaw_ - yaw_);
    tilt_ += 0.04f * (targetTilt_ - tilt_);
}

}  // namespace plumnin
