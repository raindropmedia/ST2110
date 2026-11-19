#include "core/AudioFormat.h"

#include <array>
#include <cmath>
#include <sstream>

namespace st2110 {
namespace {

struct PtimeInfo {
    PacketTime ptime;
    std::string_view name;
    std::string_view alt;
    double denominatorPerSecond; // Pakete je Sekunde
};

constexpr std::array<PtimeInfo, 5> kPtimes{{
    {PacketTime::P125us, "0.125", "125us", 8000.0},
    {PacketTime::P250us, "0.25", "250us", 4000.0},
    {PacketTime::P333us, "0.333", "333us", 3000.0},
    {PacketTime::P1ms, "1", "1ms", 1000.0},
    {PacketTime::P4ms, "4", "4ms", 250.0},
}};

const PtimeInfo& info(PacketTime ptime) {
    for (const auto& entry : kPtimes) {
        if (entry.ptime == ptime) {
            return entry;
        }
    }
    return kPtimes[3];
}

} // namespace

std::optional<PacketTime> parsePacketTime(std::string_view text) {
    for (const auto& entry : kPtimes) {
        if (entry.name == text || entry.alt == text) {
            return entry.ptime;
        }
    }
    // Schreibweisen aus SDP (a=ptime:0.12, 1.00, 0.33)
    if (text == "0.12" || text == "0.13") {
        return PacketTime::P125us;
    }
    if (text == "0.33") {
        return PacketTime::P333us;
    }
    if (text == "1.00" || text == "1.0") {
        return PacketTime::P1ms;
    }
    if (text == "4.00" || text == "4.0") {
        return PacketTime::P4ms;
    }
    return std::nullopt;
}

std::string packetTimeName(PacketTime ptime) {
    return std::string(info(ptime).alt);
}

double packetTimeNs(PacketTime ptime) {
    return 1e9 / info(ptime).denominatorPerSecond;
}

uint32_t AudioFormat::samplesPerPacket() const {
    return static_cast<uint32_t>(std::lround(sampleRate / info(ptime).denominatorPerSecond));
}

std::string AudioFormat::name() const {
    std::ostringstream out;
    out << 'L' << static_cast<int>(bitDepth) << '/' << sampleRate << '/' << channels << ' '
        << packetTimeName(ptime);
    return out.str();
}

std::string validateAudioFormat(const AudioFormat& f) {
    if (f.sampleRate != 48000 && f.sampleRate != 96000) {
        return "Abtastrate muss 48000 oder 96000 sein.";
    }
    if (f.bitDepth != 16 && f.bitDepth != 24) {
        return "Bittiefe muss 16 oder 24 sein.";
    }
    if (f.channels < 1 || f.channels > 64) {
        return "Kanalzahl muss zwischen 1 und 64 liegen.";
    }
    // Nutzlast muss in ein Ethernet-Paket passen (MTU 1500 abzüglich IP/UDP/RTP).
    if (f.payloadBytes() > 1440) {
        return "Paket zu groß (" + std::to_string(f.payloadBytes()) +
               " Byte Nutzlast): weniger Kanäle oder kürzere Paketzeit wählen.";
    }
    return {};
}

} // namespace st2110
