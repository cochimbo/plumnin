#pragma once

#include <array>
#include <cstddef>

namespace plumnin {

// What the frames-per-second counter shows: how many of the console's frames run each
// second of real time (the game's speed), how many pictures reach the window, and how
// long each frame takes to emulate and the whole loop to do its work. main() reports
// each turn of its loop; the UI draws the numbers and their history as a graph.
class FpsCounter {
public:
    // The graph's points: one every kStep seconds, kPoints of them (the last 10 seconds).
    static constexpr double kStep = 0.1;
    static constexpr int kPoints = 100;
    struct Point {
        float fps = 0;     // the console's frames per second in that step
        float emuMs = 0;   // milliseconds to emulate one of them, on average
        float workMs = 0;  // milliseconds of work per turn of the loop (emulation, drawing, menus)
    };

    // One turn of the loop, `seconds` after the last: `frames` console frames run,
    // `emuSeconds` spent running them, `workSeconds` spent in all (without the wait for
    // the next tick). `target` is the frames per second the console should make now.
    void tick(double seconds, int frames, double emuSeconds, double workSeconds, double target) {
        target_ = target;
        stepTime_ += seconds;
        stepFrames_ += frames;
        stepEmu_ += emuSeconds;
        stepWork_ += workSeconds;
        ++stepTurns_;
        if (stepTime_ < kStep) return;
        Point p;
        p.fps = static_cast<float>(stepFrames_ / stepTime_);
        p.emuMs = stepFrames_ ? static_cast<float>(stepEmu_ * 1000.0 / stepFrames_) : 0.0f;
        p.workMs = static_cast<float>(stepWork_ * 1000.0 / stepTurns_);
        points_[static_cast<size_t>(next_)] = p;
        next_ = (next_ + 1) % kPoints;
        if (count_ < kPoints) ++count_;
        // The numbers in text change twice a second, so that they can be read.
        textTime_ += stepTime_;
        textFrames_ += stepFrames_;
        textTurns_ += stepTurns_;
        textEmu_ += stepEmu_;
        textWork_ += stepWork_;
        if (textTime_ >= 0.5) {
            fps_ = textFrames_ / textTime_;
            screenFps_ = textTurns_ / textTime_;
            emuMs_ = textFrames_ ? textEmu_ * 1000.0 / textFrames_ : 0.0;
            workMs_ = textWork_ * 1000.0 / textTurns_;
            textTime_ = textEmu_ = textWork_ = 0;
            textFrames_ = textTurns_ = 0;
        }
        stepTime_ = stepEmu_ = stepWork_ = 0;
        stepFrames_ = stepTurns_ = 0;
    }
    // Starts again (a game was loaded, or the counter switched on).
    void clear() { *this = FpsCounter(); }

    double fps() const { return fps_; }              // the console's frames per second
    double screenFps() const { return screenFps_; }  // pictures shown per second
    double emuMs() const { return emuMs_; }          // milliseconds to emulate a frame
    double workMs() const { return workMs_; }        // milliseconds of work per picture shown
    double target() const { return target_; }
    // The i-th point of the graph, 0 the oldest, count() - 1 the newest.
    int count() const { return count_; }
    const Point& point(int i) const {
        return points_[static_cast<size_t>((next_ - count_ + i + kPoints) % kPoints)];
    }

private:
    std::array<Point, kPoints> points_{};
    int next_ = 0, count_ = 0;
    double stepTime_ = 0, stepEmu_ = 0, stepWork_ = 0;
    int stepFrames_ = 0, stepTurns_ = 0;
    double textTime_ = 0, textEmu_ = 0, textWork_ = 0;
    int textFrames_ = 0, textTurns_ = 0;
    double fps_ = 0, screenFps_ = 0, emuMs_ = 0, workMs_ = 0, target_ = 60;
};

}  // namespace plumnin
