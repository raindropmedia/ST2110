#include "discovery/Discovery.h"

#include "core/PacketParser.h"
#include "core/SapDirectory.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace st2110 {
namespace {

constexpr size_t kSnapLength = 160; // reicht für Ethernet, VLAN, IPv4, UDP, RTP und RFC-4175-Köpfe
constexpr unsigned kBatch = 64;

std::string errnoText() {
    return std::strerror(errno);
}

} // namespace

Discovery::Discovery(MonitorHub& hub, std::string interfaceName, bool sap)
    : hub_(hub), interface_(std::move(interfaceName)), sap_(sap) {}

Discovery::~Discovery() {
    stop();
}

bool Discovery::start(std::string& error) {
    const unsigned index = if_nametoindex(interface_.c_str());
    if (index == 0) {
        error = "Schnittstelle " + interface_ + " nicht gefunden.";
        return false;
    }

    // Mitlesen aller Rahmen
    captureSocket_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    std::string captureError;
    if (captureSocket_ < 0) {
        captureError = "Mitlesen nicht möglich (" + errnoText() + "; CAP_NET_RAW nötig).";
    } else {
        sockaddr_ll addr{};
        addr.sll_family = AF_PACKET;
        addr.sll_protocol = htons(ETH_P_ALL);
        addr.sll_ifindex = static_cast<int>(index);
        packet_mreq promisc{};
        promisc.mr_ifindex = static_cast<int>(index);
        promisc.mr_type = PACKET_MR_PROMISC;
        int bufferSize = 64 * 1024 * 1024;
        setsockopt(captureSocket_, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
        if (bind(captureSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            captureError = "Mitlesen: bind fehlgeschlagen (" + errnoText() + ").";
            close(captureSocket_);
            captureSocket_ = -1;
        } else {
            setsockopt(captureSocket_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &promisc, sizeof(promisc));
        }
    }

    // SAP
    std::string sapError;
    if (sap_) {
        sapSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
        int on = 1;
        setsockopt(sapSocket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9875);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        ip_mreqn group{};
        inet_pton(AF_INET, "239.255.255.255", &group.imr_multiaddr);
        group.imr_ifindex = static_cast<int>(index);
        if (sapSocket_ < 0 || bind(sapSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            setsockopt(sapSocket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) < 0) {
            sapError = "SAP-Empfang nicht möglich (" + errnoText() + ").";
            if (sapSocket_ >= 0) {
                close(sapSocket_);
            }
            sapSocket_ = -1;
        }
    }

    hub_.setDiscoveryStatus(nlohmann::json{
        {"interface", interface_},
        {"active", captureSocket_ >= 0},
        {"sap", sapSocket_ >= 0},
        {"error", captureError.empty() ? nlohmann::json(nullptr) : nlohmann::json(captureError)},
        {"sapError", sapError.empty() ? nlohmann::json(nullptr) : nlohmann::json(sapError)}});
    if (!captureError.empty()) {
        hub_.addEvent("warn", "Erkennung auf " + interface_ + ": " + captureError);
    }
    if (!sapError.empty()) {
        hub_.addEvent("warn", "Erkennung auf " + interface_ + ": " + sapError);
    }
    if (captureSocket_ < 0 && sapSocket_ < 0) {
        error = captureError.empty() ? sapError : captureError;
        return false;
    }
    if (captureSocket_ >= 0) {
        captureThread_ = std::thread([this] { captureLoop(); });
    }
    if (sapSocket_ >= 0) {
        sapThread_ = std::thread([this] { sapLoop(); });
    }
    hub_.addEvent("info", "Erkennung auf " + interface_ + " gestartet");
    return true;
}

void Discovery::stop() {
    stopping_ = true;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (sapThread_.joinable()) {
        sapThread_.join();
    }
    if (captureSocket_ >= 0) {
        close(captureSocket_);
        captureSocket_ = -1;
    }
    if (sapSocket_ >= 0) {
        close(sapSocket_);
        sapSocket_ = -1;
    }
}

void Discovery::captureLoop() {
    std::array<std::array<uint8_t, kSnapLength>, kBatch> buffers{};
    std::array<iovec, kBatch> iov{};
    std::array<mmsghdr, kBatch> messages{};
    std::array<sockaddr_ll, kBatch> sources{};
    auto lastStatus = std::chrono::steady_clock::now();
    uint64_t drops = 0;
    while (!stopping_) {
        pollfd pfd{captureSocket_, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        for (unsigned i = 0; i < kBatch; ++i) {
            iov[i] = iovec{buffers[i].data(), kSnapLength};
            messages[i] = mmsghdr{};
            messages[i].msg_hdr.msg_iov = &iov[i];
            messages[i].msg_hdr.msg_iovlen = 1;
            messages[i].msg_hdr.msg_name = &sources[i];
            messages[i].msg_hdr.msg_namelen = sizeof(sockaddr_ll);
        }
        // MSG_TRUNC: msg_len enthält die Länge auf der Leitung, nicht die erfasste.
        const int n = recvmmsg(captureSocket_, messages.data(), kBatch, MSG_DONTWAIT | MSG_TRUNC, nullptr);
        for (int i = 0; i < n; ++i) {
            // Eigene Sendungen (und auf lo die zweite Kopie jedes Pakets) nicht zählen.
            if (sources[i].sll_pkttype == PACKET_OUTGOING) {
                continue;
            }
            const uint32_t wire = messages[i].msg_len;
            PacketInfo info;
            if (parseEthernetFrame(buffers[i].data(), std::min<size_t>(wire, kSnapLength), wire, info)) {
                hub_.onDiscoveredPacket(info);
            }
        }
        frames_ += n > 0 ? static_cast<uint64_t>(n) : 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus > std::chrono::seconds(2)) {
            tpacket_stats stats{};
            socklen_t len = sizeof(stats);
            if (getsockopt(captureSocket_, SOL_PACKET, PACKET_STATISTICS, &stats, &len) == 0) {
                drops += stats.tp_drops; // Zähler wird beim Lesen zurückgesetzt
            }
            hub_.setDiscoveryStatus(nlohmann::json{{"interface", interface_},
                                                   {"active", true},
                                                   {"sap", sapSocket_ >= 0},
                                                   {"frames", frames_.load()},
                                                   {"drops", drops},
                                                   {"error", nullptr},
                                                   {"sapError", nullptr}});
            lastStatus = now;
        }
    }
}

void Discovery::sapLoop() {
    std::array<uint8_t, 4096> buffer{};
    while (!stopping_) {
        pollfd pfd{sapSocket_, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        const ssize_t n = recv(sapSocket_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n <= 0) {
            continue;
        }
        SapMessage message;
        std::string error;
        if (parseSapPacket(buffer.data(), static_cast<size_t>(n), message, error)) {
            hub_.onSapMessage(message);
        }
    }
}

} // namespace st2110
