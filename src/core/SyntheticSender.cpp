#include "core/SyntheticSender.h"

#include <algorithm>
#include <cmath>

namespace st2110 {

int packetsPerFrame(const VideoFormat& format) {
    const double lines = format.interlaced ? format.height / 2.0 : format.height;
    const double bytes = format.width * lines * 2.5; // 4:2:2 10 Bit: 5 Byte je 2 Pixel
    return static_cast<int>(std::ceil(bytes / 1200.0));
}

SyntheticSender::SyntheticSender(const VideoFormat& video, const AudioFormat& audio, uint64_t seed)
    : video_(video), audio_(audio),
      params_(computeVideoTimingParams(video.height, video.interlaced, video.rate, packetsPerFrame(video))),
      audioPacketNs_(packetTimeNs(audio.ptime)), rng_(seed) {}

int SyntheticSender::videoFrame(uint64_t frame, const SenderFaults& faults, double extraDelayNs,
                                const VideoEmit& emit) {
    std::normal_distribution<double> jitter(0.0, std::max(faults.jitterNs, 1e-9));
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const long double epochStart = static_cast<long double>(frame) * params_.frameTimeNs;
    const long double ticksPerFrame = static_cast<long double>(params_.frameTimeNs) * 90000.0L / 1e9L;
    const auto rtp =
        static_cast<uint32_t>(static_cast<uint64_t>(static_cast<long double>(frame) * ticksPerFrame + 1e-6L));
    // Ideal: erstes Paket 20 µs vor TR_OFFSET (Reserve für Netzlaufzeit und Pfad R, VRX bleibt unter
    // der narrow-Grenze), dann alle TRS ein Paket.
    const long double first = epochStart + params_.trOffsetNs - 20000.0 + faults.startOffsetNs + extraDelayNs;
    int lost = 0;
    uint64_t previous = 0;
    for (int i = 0; i < params_.packetsPerFrame; ++i) {
        long double t = first + static_cast<long double>(params_.trsNs) * i;
        if (faults.burstPackets > 1) {
            // Bündel: alle Pakete eines Bündels kommen zur Sollzeit seines ersten Pakets.
            t = first + static_cast<long double>(params_.trsNs) * (i - i % faults.burstPackets);
        }
        if (faults.jitterNs > 0) {
            t += jitter(rng_);
        }
        auto ns = static_cast<uint64_t>(t);
        ns = std::max(ns, previous + 1); // Reihenfolge bleibt erhalten
        previous = ns;
        if (faults.lossRate > 0 && uniform(rng_) < faults.lossRate) {
            ++lost;
            continue;
        }
        emit(rtp, ns, i);
    }
    return lost;
}

bool SyntheticSender::audioPacket(uint64_t packet, const SenderFaults& faults, double extraDelayNs,
                                  const AudioEmit& emit) {
    std::normal_distribution<double> jitter(0.0, std::max(faults.audioJitterUs * 1000.0, 1e-9));
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const auto rtp = static_cast<uint32_t>(packet * audio_.samplesPerPacket());
    // Ein Paket verlässt den Sender nach seiner Paketzeit (Abtastwerte müssen vorliegen).
    long double t = static_cast<long double>(packet) * audioPacketNs_ + audioPacketNs_ * 0.5 +
                    faults.audioDelayUs * 1000.0 + extraDelayNs;
    if (faults.audioJitterUs > 0) {
        t += std::fabs(jitter(rng_));
    }
    if (faults.lossRate > 0 && uniform(rng_) < faults.lossRate) {
        return false;
    }
    emit(rtp, static_cast<uint64_t>(t));
    return true;
}

} // namespace st2110
