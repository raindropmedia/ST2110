#pragma once

#include "core/AudioFormat.h"
#include "core/Sdp.h"
#include "core/VideoTimingModel.h"

#include <cstdint>
#include <functional>
#include <random>

namespace st2110 {

// Paketzahl je Bild bzw. Halbbild für YCbCr 4:2:2 10 Bit mit 1200 Byte Nutzlast (BPM wie MTL).
int packetsPerFrame(const VideoFormat& format);

// Störungen eines simulierten Senders (Demo, Lehre, Tests).
struct SenderFaults {
    double jitterNs = 0;       // Standardabweichung der Paketabstände (normalverteilt)
    int burstPackets = 0;      // >0: Pakete in Bündeln dieser Größe (Cinst steigt)
    double startOffsetNs = 0;  // Verschiebung des ersten Pakets gegenüber TR_OFFSET (+ = später)
    double lossRate = 0;       // Anteil verlorener Pakete
    double pathSkewNs = 12000; // Pfad R kommt so viel später an als P
    double audioJitterUs = 0;  // Standardabweichung der Tonpakete
    double audioDelayUs = 0;   // feste Verzögerung der Tonpakete gegenüber der RTP-Sollzeit
};

// Erzeugt Ankunftszeiten eines Senders nach ST 2110-21 (gapped) bzw. ST 2110-30 auf Basis der
// gemeinsamen PTP-Zeit. Ohne Störungen entsteht ein idealer narrow-Sender.
class SyntheticSender {
  public:
    using VideoEmit = std::function<void(uint32_t rtp, uint64_t arrivalNs, int index)>;
    using AudioEmit = std::function<void(uint32_t rtp, uint64_t arrivalNs)>;

    SyntheticSender(const VideoFormat& video, const AudioFormat& audio, uint64_t seed = 1);

    const VideoTimingParams& videoParams() const { return params_; }

    // Alle Pakete des Bildes mit Epochennummer frame; extraDelayNs verschiebt alles (Pfad R).
    // Liefert die Zahl der verlorenen Pakete.
    int videoFrame(uint64_t frame, const SenderFaults& faults, double extraDelayNs, const VideoEmit& emit);
    // Tonpaket mit Nummer packet (Epoche der Paketzeit). Liefert false, wenn verloren.
    bool audioPacket(uint64_t packet, const SenderFaults& faults, double extraDelayNs, const AudioEmit& emit);

  private:
    VideoFormat video_;
    AudioFormat audio_;
    VideoTimingParams params_;
    double audioPacketNs_;
    std::mt19937_64 rng_;
};

} // namespace st2110
