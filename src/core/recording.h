#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plumnin {

// Records a game to a file while it is played: an animated GIF (the picture only, at
// most 30 frames a second) or an AVI video with its sound (Motion JPEG and 16-bit PCM),
// which Windows, VLC and video editors open without installing anything. AVIs go past
// 1 GB with the OpenDML extension. Frames are encoded on threads of their own, so the
// game only waits for them if the computer cannot keep up.
class GameRecording {
public:
    enum class Format { Gif, Avi };

    GameRecording();
    ~GameRecording();  // finishes the file
    GameRecording(const GameRecording&) = delete;
    GameRecording& operator=(const GameRecording&) = delete;

    // Starts writing `path`. Every frame is scaled to width x height; fps is how many
    // frames a second addFrame() gets, and sampleRate the rate of addAudio()'s sound.
    bool start(const std::string& path, Format format, int width, int height, double fps, int sampleRate,
               std::string* error = nullptr);
    bool active() const;
    Format format() const;
    const std::string& path() const;

    // Sound heard since the last frame: interleaved stereo samples. A GIF ignores it.
    void addAudio(const int16_t* samples, size_t count);
    // One frame, 0xAARRGGBB pixels (alpha ignored) with `stride` pixels from one row to
    // the next; a negative stride reads the rows bottom up, as OpenGL returns them.
    // The sound added before it is the sound of this frame.
    void addFrame(const uint32_t* pixels, int width, int height, ptrdiff_t stride);

    // Seconds recorded so far, and whether writing has failed (error() says why).
    double seconds() const;
    bool failed() const;
    std::string error() const;
    // Ends the file. False, with the reason, if it could not be written.
    bool finish(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Scales a picture to dstWidth x dstHeight: each pixel is the average of the source
// pixels it covers, so whole-number reductions are exact and enlargements are sharp.
void scalePicture(const uint32_t* src, int srcWidth, int srcHeight, ptrdiff_t stride, uint32_t* dst, int dstWidth,
                  int dstHeight);

}  // namespace plumnin
