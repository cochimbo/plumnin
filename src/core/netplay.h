#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pad.h"

namespace plumnin {

class Console;

// Where the session's packets go and come from: a UDP socket in the programs, a
// simulated link in the tests. Packets may be lost, repeated or reordered; the
// session copes with all three.
class NetTransport {
public:
    virtual ~NetTransport() = default;
    virtual void send(const uint8_t* data, size_t size) = 0;
    // The next packet that arrived, false when there is none waiting.
    virtual bool receive(std::vector<uint8_t>& packet) = 0;
    // The host has accepted a guest: from now on only talk to whoever sent the last
    // packet received.
    virtual void keepPeer() {}
};

// The two players' pads in 12 bits, for the packets.
uint16_t packPad(const PadState& p);
PadState unpackPad(uint16_t bits);

struct NetplayConfig {
    int inputDelay = 2;   // frames between pressing a button and it taking effect
    int maxRollback = 8;  // frames run on guessed pads before waiting for the other side
};

// Two consoles playing one game over the internet with rollback, as GGPO does. Both
// run the same deterministic emulation from the same savestate. Each feeds its own
// player's pad a few frames ahead (the input delay) and sends it; the other player's
// pad is guessed (the last one received) until it arrives. When a guess turns out
// wrong, the console goes back to the savestate of that frame and runs again up to
// now with the right pads, within one display tick, so neither player waits for the
// network unless it falls more than `maxRollback` frames behind.
//
// The host is player 1 and has the game; the guest is player 2 and must have opened
// the same game. When the guest connects, the host sends it a savestate of the moment
// (so cartridge saves, region and pad types are the host's), and both start from it.
class NetplaySession {
public:
    enum class Role { Host, Guest };
    enum class Phase {
        Waiting,     // host: nobody has connected yet (the host may go on playing alone)
        Connecting,  // guest: asking the host
        Syncing,     // the host's savestate is on its way
        Playing,
        Ended,       // see endReason()
    };
    using Config = NetplayConfig;
    static constexpr uint8_t kProtocolVersion = 1;

    NetplaySession(NetTransport& transport, Role role, Config config = {});

    // Reads and answers packets; call it often (every display tick at least). `nowMs`
    // is any millisecond clock. It may change `md`: the host takes its savestate here
    // when a guest arrives and the guest loads it.
    void poll(Console& md, uint32_t nowMs);
    // Runs the next frame with `local` as this player's pad, after going back and
    // running again any frames whose guessed pads were wrong. False when no frame ran:
    // not playing yet, or too far ahead of the other player. After a true, md.audio()
    // holds only the new frame's sound (that of the frames run again is dropped).
    bool advance(Console& md, const PadState& local);
    // Runs again the frames whose guessed pads were wrong, without a new frame.
    void catchUp(Console& md);
    // True when this side is ahead of the other and should let one display tick pass
    // without a frame, so the two stay level instead of one side waiting all the time.
    bool shouldWait();
    // Leaves the session, telling the other side.
    void end();

    Role role() const { return role_; }
    Phase phase() const { return phase_; }
    const std::string& endReason() const { return endReason_; }
    const Config& config() const { return config_; }
    int localPort() const { return role_ == Role::Host ? 0 : 1; }
    // Frames run so far; the next one to run.
    uint32_t frame() const { return frame_; }
    // Frames whose pads both sides have and which will not run again.
    uint32_t confirmedFrames() const;
    // How far the host's savestate has arrived (0..1), while syncing.
    float syncProgress() const;
    int pingMs() const { return pingMs_; }
    int lastRollback() const { return lastRollback_; }  // frames run again on the last advance
    int framesAhead() const { return localAdvantage_; }
    // The first frame on which the two consoles were seen to differ, -1 if none.
    long desyncFrame() const { return desyncFrame_; }

    // Called after each frame runs (again or for the first time), with its number, so
    // tests can record each frame's result: the last call for a frame is the one that
    // counts.
    std::function<void(uint32_t frame, Console& md)> onFrame;

private:
    enum Type : uint8_t { Hello = 1, Welcome, Reject, Chunk, ChunkAck, Input, Bye };
    static constexpr size_t kChunkSize = 1024;
    static constexpr size_t kMaxInputsPerPacket = 200;

    void handle(const std::vector<uint8_t>& p, Console& md, uint32_t nowMs);
    void sendPacket(Type type, const std::vector<uint8_t>& body);
    void sendInputs(uint32_t nowMs);
    void sendChunks(uint32_t nowMs);
    void fail(const std::string& reason);
    void startPlaying(uint32_t nowMs);
    void runFrame(Console& md, uint32_t f, bool saveFirst);
    PadState remoteFor(uint32_t f) const;
    void addRemote(uint32_t f, uint16_t bits);
    void finalize();
    size_t ring(uint32_t f) const { return f % states_.size(); }

    NetTransport& transport_;
    Role role_;
    Config config_;
    Phase phase_;
    std::string endReason_;
    uint32_t session_ = 0;     // random number the host picks, on every packet after Welcome
    uint32_t lastHeard_ = 0;   // nowMs of the last packet from the other side
    uint32_t lastSent_ = 0;
    uint32_t phaseStart_ = 0;
    bool clockStarted_ = false;
    uint32_t now_ = 0;

    // The savestate both start from: the host's, sent in chunks.
    std::vector<uint8_t> start_;
    std::vector<bool> haveChunk_;
    uint32_t chunkCount_ = 0;
    uint32_t firstMissing_ = 0;  // host: what the guest last said; guest: its own
    uint32_t romCrc_ = 0;

    // Pads by frame. Both start with inputDelay neutral frames.
    std::vector<uint16_t> local_;
    std::vector<uint16_t> remote_;
    uint32_t remoteAck_ = 0;  // how many of our pads the other side has
    uint32_t frame_ = 0;
    uint32_t mismatch_ = UINT32_MAX;  // first frame run with a wrong guess
    uint32_t finalized_ = 0;          // frames before this are final
    int lastRollback_ = 0;
    // Per frame in flight: the savestate at its start, the guess used, a checksum.
    std::vector<std::vector<uint8_t>> states_;
    std::vector<uint16_t> guessed_;
    std::vector<uint32_t> checks_;
    std::vector<uint32_t> ranFrame_;    // which frame each ring slot's guess and checksum are for
    std::vector<uint32_t> stateFrame_;  // and its savestate (only frames run on a guess have one)

    // Desync detection: checksums of final frames, ours and theirs.
    std::map<uint32_t, uint32_t> ourChecks_, theirChecks_;
    uint32_t lastCheckSent_ = UINT32_MAX;
    long desyncFrame_ = -1;

    // Timing.
    uint32_t theirTime_ = 0, theirTimeAt_ = 0;  // their clock in their last packet, and when it came
    int pingMs_ = 0;
    uint32_t theirFrame_ = 0;
    int theirAdvantage_ = 0;
    int localAdvantage_ = 0;
    int sinceWait_ = 0;
};

}  // namespace plumnin
