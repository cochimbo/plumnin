#include "netplay.h"

#include "i18n.h"

#include <algorithm>
#include <cstddef>

#include "console.h"

namespace plumnin {

namespace {

// Every packet starts with "PN", the protocol version and its type; numbers are
// little endian.
class PacketWriter {
public:
    explicit PacketWriter(std::vector<uint8_t>& out) : out_(out) {}
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void bytes(const uint8_t* p, size_t n) { out_.insert(out_.end(), p, p + n); }

private:
    std::vector<uint8_t>& out_;
};

class PacketReader {
public:
    PacketReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t u8() {
        if (pos_ + 1 > n_) return bad();
        return p_[pos_++];
    }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | u8() << 8);
    }
    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | static_cast<uint32_t>(u16()) << 16;
    }
    const uint8_t* rest(size_t* size) {
        *size = n_ - pos_;
        return p_ + pos_;
    }
    bool ok() const { return ok_; }

private:
    uint8_t bad() {
        ok_ = false;
        pos_ = n_;
        return 0;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// What both sides compare to see that they are still running the same game.
uint32_t checksum(Console& md) { return md.checksum(); }

constexpr uint32_t kHelloEveryMs = 250;
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint32_t kSilenceTimeoutMs = 5000;
constexpr uint32_t kChunkEveryMs = 20;
constexpr uint32_t kChunkWindow = 64;
constexpr uint32_t kInputEveryMs = 15;
constexpr uint32_t kCheckEvery = 60;  // frames between checksums

}  // namespace

uint16_t packPad(const PadState& p) {
    const bool bits[12] = {p.up, p.down, p.left, p.right, p.a, p.b, p.x, p.y, p.l, p.r, p.select, p.start};
    uint16_t v = 0;
    for (int i = 0; i < 12; ++i) v = static_cast<uint16_t>(v | (bits[i] ? 1 : 0) << i);
    return v;
}

PadState unpackPad(uint16_t v) {
    PadState p;
    bool* bits[12] = {&p.up, &p.down, &p.left, &p.right, &p.a, &p.b, &p.x, &p.y, &p.l, &p.r, &p.select, &p.start};
    for (int i = 0; i < 12; ++i) *bits[i] = (v >> i & 1) != 0;
    return p;
}

NetplaySession::NetplaySession(NetTransport& transport, Role role, Config config)
    : transport_(transport), role_(role), config_(config), phase_(role == Role::Host ? Phase::Waiting : Phase::Connecting) {
    config_.inputDelay = std::clamp(config_.inputDelay, 0, 10);
    config_.maxRollback = std::clamp(config_.maxRollback, 1, 30);
}

void NetplaySession::sendPacket(Type type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> p = {'P', 'N', kProtocolVersion, type};
    p.insert(p.end(), body.begin(), body.end());
    transport_.send(p.data(), p.size());
}

void NetplaySession::fail(const std::string& reason) {
    if (phase_ == Phase::Ended) return;
    phase_ = Phase::Ended;
    endReason_ = reason;
}

void NetplaySession::end() {
    if (phase_ == Phase::Ended) return;
    if (phase_ != Phase::Waiting) {
        std::vector<uint8_t> body;
        PacketWriter(body).u32(session_);
        // UDP may lose one; three is plenty, and a lost Bye only means a timeout.
        for (int i = 0; i < 3; ++i) sendPacket(Bye, body);
    }
    fail(tr("Has salido de la partida."));
}

void NetplaySession::poll(Console& md, uint32_t nowMs) {
    now_ = nowMs;
    if (!clockStarted_) {
        clockStarted_ = true;
        phaseStart_ = lastHeard_ = nowMs;
        romCrc_ = md.romCrc();
    }
    std::vector<uint8_t> packet;
    while (transport_.receive(packet)) {
        if (phase_ == Phase::Ended) continue;
        handle(packet, md, nowMs);
    }
    if (phase_ == Phase::Ended || phase_ == Phase::Waiting) return;

    const uint32_t silence = nowMs - lastHeard_;
    if (phase_ == Phase::Connecting) {
        if (nowMs - phaseStart_ > kConnectTimeoutMs) {
            fail(tr("El anfitrión no responde. Comprueba la dirección y que tenga el puerto UDP abierto."));
            return;
        }
        if (nowMs - lastSent_ >= kHelloEveryMs) {
            std::vector<uint8_t> body;
            PacketWriter(body).u32(romCrc_);
            sendPacket(Hello, body);
            lastSent_ = nowMs;
        }
        return;
    }
    if (silence > kSilenceTimeoutMs) {
        fail(tr("Se ha perdido la conexión con el otro jugador."));
        return;
    }
    if (phase_ == Phase::Syncing) {
        if (role_ == Role::Host) sendChunks(nowMs);
        else if (nowMs - lastSent_ >= kChunkEveryMs) {
            std::vector<uint8_t> body;
            PacketWriter w(body);
            w.u32(session_);
            w.u32(firstMissing_);
            sendPacket(ChunkAck, body);
            lastSent_ = nowMs;
        }
        return;
    }
    // Playing: keep our pads flowing even while stalled, and estimate how far ahead we are.
    if (nowMs - lastSent_ >= kInputEveryMs) sendInputs(nowMs);
    const double framesPerMs = md.refreshRate() / 1000.0;
    const double theirNow = theirFrame_ + (pingMs_ / 2.0 + (nowMs - theirTimeAt_)) * framesPerMs;
    localAdvantage_ = static_cast<int>(static_cast<double>(frame_) - theirNow);
}

void NetplaySession::handle(const std::vector<uint8_t>& p, Console& md, uint32_t nowMs) {
    if (p.size() < 4 || p[0] != 'P' || p[1] != 'N') return;
    const Type type = static_cast<Type>(p[3]);
    if (p[2] != kProtocolVersion) {
        // A different plumnin. Tell a guest why instead of letting it time out.
        if (role_ == Role::Host && type == Hello && phase_ == Phase::Waiting) {
            std::vector<uint8_t> body;
            PacketWriter(body).u8(1);
            sendPacket(Reject, body);
        } else if (role_ == Role::Guest && phase_ == Phase::Connecting) {
            fail(tr("El anfitrión usa otra versión de plumnin: tenéis que usar la misma."));
        }
        return;
    }
    PacketReader r(p.data() + 4, p.size() - 4);

    if (role_ == Role::Host && type == Hello) {
        const uint32_t crc = r.u32();
        if (!r.ok()) return;
        if (phase_ == Phase::Syncing) {
            // Our Welcome was lost.
        } else if (phase_ != Phase::Waiting) {
            return;
        } else if (crc != md.romCrc()) {
            std::vector<uint8_t> body;
            PacketWriter(body).u8(2);
            sendPacket(Reject, body);
            return;
        } else {
            // The guest is in: freeze the game here and send it this moment. The host
            // loads it too, so both consoles have been through the same loadState().
            transport_.keepPeer();
            std::string error;
            if (!md.saveState(start_) || !md.loadState(start_, &error)) {
                fail(trf("No se pudo preparar la partida: %s", error.c_str()));
                return;
            }
            session_ = (nowMs * 2654435761u) ^ crc ^ static_cast<uint32_t>(start_.size()) ^ 0x504C554Du;
            if (session_ == 0) session_ = 1;
            chunkCount_ = static_cast<uint32_t>((start_.size() + kChunkSize - 1) / kChunkSize);
            firstMissing_ = 0;
            phase_ = Phase::Syncing;
            phaseStart_ = nowMs;
        }
        lastHeard_ = nowMs;
        std::vector<uint8_t> body;
        PacketWriter w(body);
        w.u32(session_);
        w.u32(static_cast<uint32_t>(start_.size()));
        w.u32(chunkCount_);
        w.u8(static_cast<uint8_t>(config_.inputDelay));
        w.u8(static_cast<uint8_t>(config_.maxRollback));
        sendPacket(Welcome, body);
        return;
    }
    if (role_ == Role::Guest && type == Reject && phase_ == Phase::Connecting) {
        const uint8_t why = r.u8();
        fail(why == 2 ? tr("El anfitrión está jugando a otro juego: abrid los dos el mismo (la misma ROM).")
             : why == 1 ? tr("El anfitrión usa otra versión de plumnin: tenéis que usar la misma.")
                        : tr("El anfitrión no ha aceptado la conexión."));
        return;
    }
    if (role_ == Role::Guest && type == Welcome && phase_ == Phase::Connecting) {
        session_ = r.u32();
        const uint32_t size = r.u32();
        chunkCount_ = r.u32();
        config_.inputDelay = r.u8();
        config_.maxRollback = std::max<int>(1, r.u8());
        if (!r.ok() || size == 0 || size > (64u << 20) || chunkCount_ != (size + kChunkSize - 1) / kChunkSize) return;
        start_.assign(size, 0);
        haveChunk_.assign(chunkCount_, false);
        firstMissing_ = 0;
        phase_ = Phase::Syncing;
        phaseStart_ = lastHeard_ = nowMs;
        return;
    }

    // Everything else belongs to the session the Welcome started.
    if (r.u32() != session_ || !r.ok() || phase_ == Phase::Waiting || phase_ == Phase::Connecting) return;
    lastHeard_ = nowMs;
    switch (type) {
        case Bye:
            fail(tr("El otro jugador ha salido de la partida."));
            return;
        case Chunk: {
            if (role_ != Role::Guest || phase_ != Phase::Syncing) return;
            const uint32_t index = r.u32();
            size_t n = 0;
            const uint8_t* data = r.rest(&n);
            const size_t at = static_cast<size_t>(index) * kChunkSize;
            if (!r.ok() || index >= chunkCount_ || n != std::min(kChunkSize, start_.size() - at)) return;
            std::copy(data, data + n, start_.begin() + static_cast<std::ptrdiff_t>(at));
            haveChunk_[index] = true;
            const uint32_t before = firstMissing_;
            while (firstMissing_ < chunkCount_ && haveChunk_[firstMissing_]) ++firstMissing_;
            if (firstMissing_ < chunkCount_) {
                if (firstMissing_ != before) lastSent_ = 0;  // ack progress soon
                return;
            }
            std::string error;
            if (!md.loadState(start_, &error)) {
                end();
                fail(trf("No se pudo cargar la partida del anfitrión: %s", error.c_str()));
                return;
            }
            startPlaying(nowMs);
            return;
        }
        case ChunkAck: {
            if (role_ != Role::Host) return;
            const uint32_t missing = r.u32();
            if (!r.ok()) return;
            if (phase_ == Phase::Syncing) {
                firstMissing_ = std::max(firstMissing_, std::min(missing, chunkCount_));
                if (firstMissing_ >= chunkCount_) startPlaying(nowMs);
            }
            return;
        }
        case Input: {
            if (phase_ == Phase::Syncing) {
                if (role_ != Role::Host) return;
                startPlaying(nowMs);  // the guest has it all and is playing already
            }
            if (phase_ != Phase::Playing) return;
            const uint32_t ack = r.u32();
            const uint32_t sentAt = r.u32();
            const uint32_t echo = r.u32();
            const uint16_t hold = r.u16();
            const uint32_t theirFrame = r.u32();
            const int8_t advantage = static_cast<int8_t>(r.u8());
            const uint32_t checkFrame = r.u32();
            const uint32_t check = r.u32();
            const uint32_t first = r.u32();
            const uint16_t count = r.u16();
            if (!r.ok()) return;
            std::vector<uint16_t> pads(count);
            for (uint16_t& v : pads) v = r.u16();
            if (!r.ok()) return;
            remoteAck_ = std::max(remoteAck_, std::min<uint32_t>(ack, static_cast<uint32_t>(local_.size())));
            if (sentAt - theirTime_ < 0x80000000u || theirTimeAt_ == 0) {
                theirTime_ = sentAt;
                theirTimeAt_ = nowMs;
                theirFrame_ = theirFrame;
                theirAdvantage_ = advantage;
            }
            if (echo != 0) {
                const uint32_t rtt = nowMs - echo - hold;
                if (rtt < 5000) pingMs_ = pingMs_ == 0 ? static_cast<int>(rtt) : (pingMs_ * 7 + static_cast<int>(rtt)) / 8;
            }
            if (checkFrame != UINT32_MAX) {
                theirChecks_[checkFrame] = check;
                const auto ours = ourChecks_.find(checkFrame);
                if (ours != ourChecks_.end() && ours->second != check && desyncFrame_ < 0) desyncFrame_ = checkFrame;
            }
            for (uint32_t i = 0; i < count; ++i) addRemote(first + i, pads[i]);
            return;
        }
        default:
            return;
    }
}

void NetplaySession::sendChunks(uint32_t nowMs) {
    if (nowMs - lastSent_ < kChunkEveryMs) return;
    lastSent_ = nowMs;
    const uint32_t end = std::min(chunkCount_, firstMissing_ + kChunkWindow);
    for (uint32_t i = firstMissing_; i < end; ++i) {
        std::vector<uint8_t> body;
        PacketWriter w(body);
        w.u32(session_);
        w.u32(i);
        const size_t at = static_cast<size_t>(i) * kChunkSize;
        w.bytes(start_.data() + at, std::min(kChunkSize, start_.size() - at));
        sendPacket(Chunk, body);
    }
}

void NetplaySession::startPlaying(uint32_t nowMs) {
    if (phase_ == Phase::Playing) return;
    phase_ = Phase::Playing;
    phaseStart_ = lastHeard_ = nowMs;
    start_.clear();
    start_.shrink_to_fit();
    haveChunk_.clear();
    const size_t delay = static_cast<size_t>(config_.inputDelay);
    local_.assign(delay, 0);
    remote_.assign(delay, 0);
    remoteAck_ = static_cast<uint32_t>(delay);
    frame_ = 0;
    finalized_ = 0;
    mismatch_ = UINT32_MAX;
    const size_t slots = static_cast<size_t>(config_.maxRollback) + 2;
    states_.assign(slots, {});
    guessed_.assign(slots, 0);
    checks_.assign(slots, 0);
    ranFrame_.assign(slots, UINT32_MAX);
    stateFrame_.assign(slots, UINT32_MAX);
    sendInputs(nowMs);
}

void NetplaySession::sendInputs(uint32_t nowMs) {
    if (phase_ != Phase::Playing) return;
    std::vector<uint8_t> body;
    PacketWriter w(body);
    w.u32(session_);
    w.u32(static_cast<uint32_t>(remote_.size()));
    w.u32(nowMs == 0 ? 1 : nowMs);
    w.u32(theirTime_);
    w.u16(static_cast<uint16_t>(std::min<uint32_t>(nowMs - theirTimeAt_, 0xFFFF)));
    w.u32(frame_);
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(localAdvantage_, -100, 100))));
    if (ourChecks_.empty()) {
        w.u32(UINT32_MAX);
        w.u32(0);
    } else {
        w.u32(ourChecks_.rbegin()->first);
        w.u32(ourChecks_.rbegin()->second);
    }
    const uint32_t first = remoteAck_;
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(local_.size()) - first, kMaxInputsPerPacket);
    w.u32(first);
    w.u16(static_cast<uint16_t>(count));
    for (uint32_t i = 0; i < count; ++i) w.u16(local_[first + i]);
    sendPacket(Input, body);
    lastSent_ = nowMs;
}

PadState NetplaySession::remoteFor(uint32_t f) const {
    if (f < remote_.size()) return unpackPad(remote_[f]);
    // A guess: they are still holding what they held last.
    return remote_.empty() ? PadState{} : unpackPad(remote_.back());
}

void NetplaySession::addRemote(uint32_t f, uint16_t bits) {
    if (f != remote_.size()) return;  // a repeat, or a gap (the next packet fills it)
    remote_.push_back(bits);
    const size_t i = ring(f);
    if (f < frame_ && ranFrame_[i] == f && guessed_[i] != bits) mismatch_ = std::min(mismatch_, f);
}

void NetplaySession::runFrame(Console& md, uint32_t f, bool saveFirst) {
    const size_t i = ring(f);
    // Only a frame run on a guessed pad can need going back to; with the pads on time
    // (the input delay covering the ping) no savestate is taken at all.
    if (saveFirst && f >= remote_.size() && md.saveState(states_[i], true)) stateFrame_[i] = f;
    const PadState local = unpackPad(local_[f]);
    const PadState remote = remoteFor(f);
    guessed_[i] = packPad(remote);
    ranFrame_[i] = f;
    if (role_ == Role::Host) md.setPads(local, remote);
    else md.setPads(remote, local);
    md.runFrame();
    if (f % kCheckEvery == 0) checks_[i] = checksum(md);
    if (onFrame) onFrame(f, md);
}

void NetplaySession::catchUp(Console& md) {
    if (phase_ != Phase::Playing) return;
    if (mismatch_ >= frame_) {
        mismatch_ = UINT32_MAX;
        finalize();
        return;
    }
    const uint32_t from = mismatch_;
    mismatch_ = UINT32_MAX;
    std::string error;
    if (stateFrame_[ring(from)] != from || !md.loadState(states_[ring(from)], &error)) {
        end();
        fail(tr("La partida se ha descompasado y no se puede recuperar."));
        return;
    }
    for (uint32_t f = from; f < frame_; ++f) {
        runFrame(md, f, f != from);
        md.audio().clearSamples();  // already heard, with the guessed pads
    }
    lastRollback_ = static_cast<int>(frame_ - from);
    finalize();
}

bool NetplaySession::advance(Console& md, const PadState& local) {
    if (phase_ != Phase::Playing) return false;
    lastRollback_ = 0;
    catchUp(md);
    if (phase_ != Phase::Playing) return false;
    // Too far ahead on guesses: wait for the other side.
    if (frame_ >= remote_.size() + static_cast<size_t>(config_.maxRollback)) return false;
    local_.push_back(packPad(local));
    runFrame(md, frame_, true);
    ++frame_;
    finalize();
    sendInputs(now_);
    return true;
}

void NetplaySession::finalize() {
    const uint32_t limit = std::min({static_cast<uint32_t>(remote_.size()), frame_, mismatch_});
    for (uint32_t f = finalized_; f < limit; ++f) {
        if (f % kCheckEvery != 0 || ranFrame_[ring(f)] != f) continue;
        const uint32_t check = checks_[ring(f)];
        ourChecks_[f] = check;
        const auto theirs = theirChecks_.find(f);
        if (theirs != theirChecks_.end() && theirs->second != check && desyncFrame_ < 0) desyncFrame_ = f;
    }
    finalized_ = std::max(finalized_, limit);
    // Keep the last few seconds of checksums.
    while (ourChecks_.size() > 16) ourChecks_.erase(ourChecks_.begin());
    while (theirChecks_.size() > 16) theirChecks_.erase(theirChecks_.begin());
}

uint32_t NetplaySession::confirmedFrames() const { return finalized_; }

float NetplaySession::syncProgress() const {
    if (phase_ == Phase::Playing) return 1.0f;
    if (phase_ != Phase::Syncing || chunkCount_ == 0) return 0.0f;
    return static_cast<float>(firstMissing_) / static_cast<float>(chunkCount_);
}

bool NetplaySession::shouldWait() {
    if (phase_ != Phase::Playing) return false;
    ++sinceWait_;
    // Each side knows its own lead; half the difference is how much one should give up.
    if ((localAdvantage_ - theirAdvantage_) / 2 >= 1 && sinceWait_ >= 10) {
        sinceWait_ = 0;
        return true;
    }
    return false;
}

}  // namespace plumnin
