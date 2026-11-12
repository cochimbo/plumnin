#include "udp.h"

#include "../core/i18n.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
using SockLen = int;
#define PLUMNIN_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
using SockLen = socklen_t;
#define PLUMNIN_BAD_SOCKET (-1)
#endif

namespace plumnin {

namespace {

#ifdef _WIN32
// Winsock wants starting once per process; it stays up until the program ends.
bool startSockets() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
void closeSocket(SocketHandle s) { closesocket(s); }
bool makeNonBlocking(SocketHandle s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
bool startSockets() { return true; }
void closeSocket(SocketHandle s) { close(s); }
bool makeNonBlocking(SocketHandle s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

SocketHandle handle(intptr_t s) { return static_cast<SocketHandle>(s); }

}  // namespace

UdpTransport::UdpTransport() : socket_(static_cast<intptr_t>(PLUMNIN_BAD_SOCKET)) {}

UdpTransport::~UdpTransport() {
    if (handle(socket_) != PLUMNIN_BAD_SOCKET) closeSocket(handle(socket_));
}

bool UdpTransport::open(int port) {
    if (!startSockets()) {
        error_ = tr("No se pudo iniciar la red.");
        return false;
    }
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PLUMNIN_BAD_SOCKET) {
        error_ = tr("No se pudo crear el socket UDP.");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        closeSocket(s);
        error_ = trf("El puerto %d está ocupado (¿hay otro plumnin alojando?).", port);
        return false;
    }
    if (!makeNonBlocking(s)) {
        closeSocket(s);
        error_ = tr("No se pudo preparar el socket UDP.");
        return false;
    }
    socket_ = static_cast<intptr_t>(s);
    return true;
}

bool UdpTransport::setPeer(const std::string& address, int defaultPort) {
    if (!startSockets()) {
        error_ = tr("No se pudo iniciar la red.");
        return false;
    }
    std::string host = address;
    int port = defaultPort;
    // Spaces from copy and paste.
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.pop_back();
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.erase(host.begin());
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        char* end = nullptr;
        const long p = std::strtol(host.c_str() + colon + 1, &end, 10);
        if (*end != 0 || p <= 0 || p > 65535) {
            error_ = trf("El puerto de \"%s\" no es válido.", address.c_str());
            return false;
        }
        port = static_cast<int>(p);
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        error_ = tr("Escribe la dirección del anfitrión.");
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
        error_ = trf("No se encuentra \"%s\".", host.c_str());
        return false;
    }
    peerIp_ = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    peerPort_ = htons(static_cast<uint16_t>(port));
    havePeer_ = true;
    locked_ = true;  // a guest only listens to its host
    return true;
}

int UdpTransport::localPort() const {
    if (handle(socket_) == PLUMNIN_BAD_SOCKET) return 0;
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (getsockname(handle(socket_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

void UdpTransport::send(const uint8_t* data, size_t size) {
    if (handle(socket_) == PLUMNIN_BAD_SOCKET || !havePeer_) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peerIp_;
    to.sin_port = peerPort_;
    // A full send buffer just loses the packet, as the network could.
    ::sendto(handle(socket_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
             reinterpret_cast<sockaddr*>(&to), sizeof to);
}

bool UdpTransport::receive(std::vector<uint8_t>& packet) {
    if (handle(socket_) == PLUMNIN_BAD_SOCKET) return false;
    for (;;) {
        uint8_t buffer[2048];
        sockaddr_in from{};
        SockLen len = sizeof from;
        const auto n = ::recvfrom(handle(socket_), reinterpret_cast<char*>(buffer), sizeof buffer, 0,
                                  reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) {
#ifdef _WIN32
            // Windows reports an earlier packet that found no one listening as an
            // error on the next read (WSAECONNRESET); it is not fatal, keep reading.
            if (WSAGetLastError() == WSAECONNRESET) continue;
#endif
            return false;
        }
        if (locked_ && (from.sin_addr.s_addr != peerIp_ || from.sin_port != peerPort_)) continue;
        if (!locked_) {
            // Until a guest is accepted, answer whoever wrote last.
            peerIp_ = from.sin_addr.s_addr;
            peerPort_ = from.sin_port;
            havePeer_ = true;
        }
        packet.assign(buffer, buffer + n);
        return true;
    }
}

std::vector<std::string> UdpTransport::localAddresses() {
    std::vector<std::string> out;
    if (!startSockets()) return out;
    // "Connecting" a UDP socket sends nothing but makes the system pick the address
    // it would use to reach the internet, which is the one on the local network.
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PLUMNIN_BAD_SOCKET) return out;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        SockLen len = sizeof me;
        char text[INET_ADDRSTRLEN] = "";
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, text, sizeof text) && std::strcmp(text, "0.0.0.0") != 0)
            out.push_back(text);
    }
    closeSocket(s);
    return out;
}

}  // namespace plumnin
