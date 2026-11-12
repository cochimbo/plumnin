#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../core/netplay.h"

namespace plumnin {

// A non-blocking IPv4 UDP socket that is a NetTransport: it sends to one peer and
// receives from anyone until keepPeer() or setPeer(), then only from that peer. The
// host binds a port and answers whoever writes first; the guest sends to the host's
// address.
class UdpTransport : public NetTransport {
public:
    UdpTransport();
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Opens the socket on `port` (0: any free one). False with a reason in error().
    bool open(int port);
    // Where to send: "1.2.3.4:7845", "name.example:7845" or just a name with
    // `defaultPort`. False with a reason in error().
    bool setPeer(const std::string& address, int defaultPort);
    int localPort() const;
    const std::string& error() const { return error_; }

    void send(const uint8_t* data, size_t size) override;
    bool receive(std::vector<uint8_t>& packet) override;
    void keepPeer() override { locked_ = havePeer_; }

    // This computer's addresses on the local network, for the host to tell a friend
    // in the same house (over the internet they need the router's public address).
    static std::vector<std::string> localAddresses();

private:
    intptr_t socket_;
    uint32_t peerIp_ = 0;     // network byte order
    uint16_t peerPort_ = 0;   // network byte order
    bool havePeer_ = false;
    bool locked_ = false;
    std::string error_;
};

}  // namespace plumnin
