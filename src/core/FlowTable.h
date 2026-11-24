#pragma once

#include "core/AudioFormat.h"
#include "core/PacketParser.h"
#include "core/Sdp.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace st2110 {

// Ein im Netz erkannter Flow (Ziel, Port, Quelle) mit Raten und geschätzter Art und Format.
struct DiscoveredFlow {
    uint32_t dstIp = 0;
    uint16_t dstPort = 0;
    uint32_t srcIp = 0;
    uint16_t srcPort = 0;
    int vlan = -1;
    uint8_t dscp = 0;
    double firstSeen = 0;
    double lastSeen = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    double pps = 0;
    double bps = 0;

    // RTP
    bool rtp = false;
    uint8_t payloadType = 0;
    uint32_t ssrc = 0;
    uint64_t sequenceErrors = 0; // fehlende Pakete laut Sequenznummer
    uint16_t payloadMin = 0;
    uint16_t payloadMax = 0;
    double clockRate = 0; // geschätzter RTP-Takt in Hz (0 = unbekannt)
    double markersPerSec = 0;

    // Schätzung
    std::string kind = "udp"; // video | audio | anc | ptp | sap | rtp | udp
    std::optional<VideoFormat> video;
    std::optional<AudioFormat> audio;
    // ST 2110-20: alle Pixelformate, die zu den gemessenen Bytes je Bildpunkt passen (Formate mit gleicher
    // Pixelgruppe sind auf der Leitung nicht zu unterscheiden, z. B. YCbCr 4:4:4 und RGB), in der Reihenfolge
    // der Häufigkeit; und der Packmodus (GPM sicher, wenn ein Paket mehrere Zeilensegmente trägt, sonst BPM
    // oder GPM).
    std::vector<PixelFormat> pixelCandidates;
    std::optional<Packing> packing;
    bool packingCertain = false;

    std::string formatGuess() const;
};

// Sammelt mitgelesene Pakete je Flow. Nicht threadsicher (der Hub sperrt).
class FlowTable {
  public:
    // count/bytes: für zusammengefasste Pakete (Demo); bytes = 0 heißt count · wireBytes.
    void observe(const PacketInfo& packet, double nowSec, uint32_t count = 1, uint64_t bytes = 0);
    // Entfernt Flows, die länger als maxAgeSec nicht gesehen wurden.
    void expire(double nowSec, double maxAgeSec = 60.0);
    std::vector<DiscoveredFlow> list(double nowSec) const;
    size_t size() const { return flows_.size(); }

  private:
    struct Entry {
        DiscoveredFlow flow;
        // Ratenfenster
        double windowStart = 0;
        uint64_t windowPackets = 0;
        uint64_t windowBytes = 0;
        uint64_t windowMarkers = 0;
        uint64_t markers = 0;
        // RTP-Takt
        bool hasClockRef = false;
        uint32_t clockRefTs = 0;
        double clockRefTime = 0;
        // Sequenz
        bool hasSeq = false;
        uint16_t nextSeq = 0;
        // RFC 4175
        uint64_t rfcPackets = 0;
        uint16_t maxRow = 0;
        bool sawSecondField = false;
        uint64_t fieldBytes = 0;
        uint64_t lastFieldBytes = 0;
        // Pixelgruppe: Verträglichkeit der Segmentlängen mit jedem der 16 Formate (aufeinanderfolgende Pakete
        // einer Zeile liefern Bytes und Bildpunkte je Segment)
        std::array<uint32_t, 16> pgroupHits{};
        uint32_t pgroupSamples = 0;
        bool hasPrevSegment = false;
        uint16_t prevRow = 0;
        uint16_t prevOffset = 0;
        uint16_t prevLength = 0;
        bool prevSecondField = false;
        bool sawMultiSegment = false;
        // letztes Segment des letzten Pakets mit Marker: Ende der letzten Zeile
        uint16_t endRow = 0;
        uint16_t endOffset = 0;
        uint16_t endLength = 0;
        // Takte je Bild bzw. Halbbild aus aufeinanderfolgenden Markern
        bool hasMarkerTs = false;
        uint32_t lastMarkerTs = 0;
        uint64_t tickSum = 0;
        uint64_t tickCount = 0;
    };
    void classify(Entry& e) const;

    std::map<std::tuple<uint32_t, uint16_t, uint32_t>, Entry> flows_;
};

// JSON eines erkannten Flows; monitoredId: ID des überwachten Flows mit derselben Adresse (oder leer).
nlohmann::json discoveredFlowToJson(const DiscoveredFlow& flow, double nowSec,
                                    const std::string& monitoredId);

} // namespace st2110
