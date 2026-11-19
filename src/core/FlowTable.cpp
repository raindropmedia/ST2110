#include "core/FlowTable.h"

#include <array>
#include <cmath>

namespace st2110 {
namespace {

// Bekannte Breiten für die Formatschätzung aus Bytes je Zeile.
constexpr std::array<uint32_t, 7> kWidths{720, 1280, 1440, 1920, 2048, 3840, 4096};
constexpr std::array<double, 4> kClocks{90000, 48000, 96000, 44100};

// Nächstliegende Bildrate; 59,94 und 60 liegen nur 0,1 % auseinander, daher der genaue Wert aus
// den RTP-Zeitstempeln (Takte je Bild), nicht aus der Zahl der Marker je Sekunde.
std::optional<FrameRate> rateFromHz(double hz) {
    std::optional<FrameRate> best;
    double bestError = 0.0005;
    for (auto r : {FrameRate::P23_98, FrameRate::P24, FrameRate::P25, FrameRate::P29_97, FrameRate::P30,
                   FrameRate::P50, FrameRate::P59_94, FrameRate::P60, FrameRate::P100, FrameRate::P119_88,
                   FrameRate::P120}) {
        const double expected = 1e9 / framePeriodNs(r);
        const double error = std::fabs(hz - expected) / expected;
        if (error < bestError) {
            bestError = error;
            best = r;
        }
    }
    return best;
}

std::optional<PacketTime> ptimeFromPps(double pps) {
    for (auto p :
         {PacketTime::P125us, PacketTime::P250us, PacketTime::P333us, PacketTime::P1ms, PacketTime::P4ms}) {
        const double expected = 1e9 / packetTimeNs(p);
        if (std::fabs(pps - expected) / expected < 0.03) {
            return p;
        }
    }
    return std::nullopt;
}

} // namespace

std::string DiscoveredFlow::formatGuess() const {
    if (video) {
        return video->name();
    }
    if (audio) {
        return audio->name();
    }
    return {};
}

void FlowTable::observe(const PacketInfo& p, double now, uint32_t count, uint64_t bytes) {
    if (count == 0) {
        return;
    }
    const uint64_t total = bytes != 0 ? bytes : static_cast<uint64_t>(p.wireBytes) * count;
    auto [it, inserted] = flows_.try_emplace(std::make_tuple(p.dstIp, p.dstPort, p.srcIp));
    Entry& e = it->second;
    DiscoveredFlow& f = e.flow;
    if (inserted) {
        f.dstIp = p.dstIp;
        f.dstPort = p.dstPort;
        f.srcIp = p.srcIp;
        f.firstSeen = now;
        e.windowStart = now;
        f.payloadMin = p.rtpPayload;
    }
    f.srcPort = p.srcPort;
    f.vlan = p.vlan;
    f.dscp = p.dscp;
    f.lastSeen = now;
    f.packets += count;
    f.bytes += total;
    e.windowPackets += count;
    e.windowBytes += total;

    if (p.rtp) {
        f.rtp = true;
        f.payloadType = p.payloadType;
        f.ssrc = p.ssrc;
        f.payloadMin = std::min(f.payloadMin, p.rtpPayload);
        f.payloadMax = std::max(f.payloadMax, p.rtpPayload);
        // Sequenzlücken (zusammengefasste Pakete zählen als lückenlos)
        if (e.hasSeq && count == 1) {
            const auto gap = static_cast<uint16_t>(p.sequence - e.nextSeq);
            if (gap != 0 && gap < 0x8000) {
                f.sequenceErrors += gap;
            }
        }
        e.nextSeq = static_cast<uint16_t>(p.sequence + count);
        e.hasSeq = true;
        // RTP-Takt aus Zeitstempel und Ankunft über mindestens 1 s
        if (!e.hasClockRef) {
            e.clockRefTs = p.timestamp;
            e.clockRefTime = now;
            e.hasClockRef = true;
        } else if (now - e.clockRefTime >= 1.0) {
            const double rate = static_cast<double>(static_cast<uint32_t>(p.timestamp - e.clockRefTs)) /
                                (now - e.clockRefTime);
            for (double c : kClocks) {
                if (std::fabs(rate - c) / c < 0.05) {
                    f.clockRate = c;
                }
            }
            e.clockRefTs = p.timestamp;
            e.clockRefTime = now;
        }
        if (p.rfc4175) {
            e.rfcPackets += count;
            e.maxRow = std::max(e.maxRow, p.row);
            e.sawSecondField = e.sawSecondField || p.secondField;
            e.fieldBytes += static_cast<uint64_t>(p.videoBytes) * count;
        }
        if (p.marker) {
            ++e.markers;
            ++e.windowMarkers;
            e.lastFieldBytes = e.fieldBytes;
            e.fieldBytes = 0;
            if (e.hasMarkerTs) {
                const uint32_t ticks = p.timestamp - e.lastMarkerTs;
                if (ticks > 0 && ticks < 90000) {
                    e.tickSum += ticks;
                    e.tickCount++;
                }
            }
            e.lastMarkerTs = p.timestamp;
            e.hasMarkerTs = true;
        }
    }

    const double dt = now - e.windowStart;
    if (dt >= 1.0) {
        f.pps = static_cast<double>(e.windowPackets) / dt;
        f.bps = static_cast<double>(e.windowBytes) * 8.0 / dt;
        f.markersPerSec = static_cast<double>(e.windowMarkers) / dt;
        e.windowStart = now;
        e.windowPackets = e.windowBytes = e.windowMarkers = 0;
        classify(e);
    } else if (inserted) {
        classify(e);
    }
}

void FlowTable::classify(Entry& e) const {
    DiscoveredFlow& f = e.flow;
    f.video.reset();
    f.audio.reset();
    if (f.dstPort == 319 || f.dstPort == 320) {
        f.kind = "ptp";
        return;
    }
    if (f.dstPort == 9875) {
        f.kind = "sap";
        return;
    }
    if (!f.rtp) {
        f.kind = "udp";
        return;
    }
    f.kind = "rtp";
    const bool mostlyRfc = f.packets > 0 && e.rfcPackets * 10 >= f.packets * 9;
    if (f.clockRate == 90000 || (f.clockRate == 0 && mostlyRfc)) {
        if (mostlyRfc) {
            f.kind = "video";
            if (e.maxRow > 0 && f.markersPerSec > 0) {
                VideoFormat v;
                v.interlaced = e.sawSecondField;
                const uint32_t lines = static_cast<uint32_t>(e.maxRow) + 1;
                v.height = v.interlaced ? lines * 2 : lines;
                const double hz = e.tickCount > 0 ? 90000.0 * static_cast<double>(e.tickCount) /
                                                        static_cast<double>(e.tickSum)
                                                  : f.markersPerSec;
                auto rate = rateFromHz(hz);
                if (e.lastFieldBytes > 0 && rate) {
                    const double width = static_cast<double>(e.lastFieldBytes) / (lines * 2.5);
                    uint32_t best = 0;
                    for (uint32_t w : kWidths) {
                        if (std::fabs(width - w) / w < 0.02) {
                            best = w;
                        }
                    }
                    if (best != 0) {
                        v.width = best;
                        v.rate = *rate;
                        f.video = v;
                    }
                }
            }
        } else if (f.pps < 2000) {
            f.kind = "anc"; // vermutlich ST 2110-40 (wenige Pakete je Bild)
        }
        return;
    }
    if (f.clockRate == 48000 || f.clockRate == 96000) {
        f.kind = "audio";
        auto ptime = ptimeFromPps(f.pps);
        if (ptime && f.payloadMax == f.payloadMin) {
            AudioFormat a;
            a.sampleRate = static_cast<uint32_t>(f.clockRate);
            a.ptime = *ptime;
            const uint32_t samples = a.samplesPerPacket();
            if (samples > 0 && f.payloadMax % samples == 0) {
                const uint32_t perSample = f.payloadMax / samples;
                if (perSample % 3 == 0) {
                    a.bitDepth = 24;
                    a.channels = static_cast<uint16_t>(perSample / 3);
                    f.audio = a;
                } else if (perSample % 2 == 0) {
                    a.bitDepth = 16;
                    a.channels = static_cast<uint16_t>(perSample / 2);
                    f.audio = a;
                }
            }
        }
    }
}

void FlowTable::expire(double now, double maxAgeSec) {
    for (auto it = flows_.begin(); it != flows_.end();) {
        if (now - it->second.flow.lastSeen > maxAgeSec) {
            it = flows_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<DiscoveredFlow> FlowTable::list(double now) const {
    std::vector<DiscoveredFlow> out;
    out.reserve(flows_.size());
    for (const auto& [key, e] : flows_) {
        DiscoveredFlow f = e.flow;
        if (now - f.lastSeen > 2.0) {
            f.pps = f.bps = f.markersPerSec = 0; // nicht mehr aktiv
        }
        out.push_back(f);
    }
    return out;
}

nlohmann::json discoveredFlowToJson(const DiscoveredFlow& f, double now, const std::string& monitoredId) {
    using nlohmann::json;
    return json{
        {"dstIp", ipv4ToString(f.dstIp)},
        {"dstPort", f.dstPort},
        {"srcIp", ipv4ToString(f.srcIp)},
        {"srcPort", f.srcPort},
        {"multicast", (f.dstIp >> 28) == 0xE},
        {"vlan", f.vlan >= 0 ? json(f.vlan) : json(nullptr)},
        {"dscp", f.dscp},
        {"kind", f.kind},
        {"format", f.formatGuess().empty() ? json(nullptr) : json(f.formatGuess())},
        {"rtp", f.rtp},
        {"payloadType", f.rtp ? json(f.payloadType) : json(nullptr)},
        {"ssrc", f.rtp ? json(f.ssrc) : json(nullptr)},
        {"clockRate", f.clockRate > 0 ? json(f.clockRate) : json(nullptr)},
        {"pps", f.pps},
        {"bps", f.bps},
        {"packets", f.packets},
        {"bytes", f.bytes},
        {"sequenceErrors", f.sequenceErrors},
        {"payloadBytes", f.rtp ? json{{"min", f.payloadMin}, {"max", f.payloadMax}} : json(nullptr)},
        {"active", now - f.lastSeen <= 2.0},
        {"lastSeenSec", now - f.lastSeen},
        {"monitored", monitoredId.empty() ? json(nullptr) : json(monitoredId)},
    };
}

} // namespace st2110
