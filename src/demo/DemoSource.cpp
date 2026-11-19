#include "demo/DemoSource.h"

#include <chrono>
#include <cmath>
#include <random>

namespace st2110 {
namespace {

// TAI liegt 37 s vor UTC (Stand 2017, unverändert).
constexpr uint64_t kTaiOffsetNs = 37ULL * 1'000'000'000ULL;

uint64_t taiNowNs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) +
           kTaiOffsetNs;
}

} // namespace

const std::vector<DemoSource::Preset>& DemoSource::presets() {
    static const std::vector<Preset> kPresets{
        {"ideal", "Ideal", "Sender nach ST 2110-21 narrow, Ton ohne Jitter, PTP eingerastet"},
        {"jitter", "Jitter", "Paketabstände mit 1,5 µs Streuung (IPT-Histogramm wird breit)"},
        {"burst", "Bündel", "Je 8 Pakete auf einmal: Cinst steigt, Urteil wide"},
        {"late", "Zu spät", "Erstes Paket 40 µs nach TR_OFFSET: FPT-Fehler, Urteil failed"},
        {"loss", "Paketverlust", "Jedes 2000. Paket fehlt auf beiden Pfaden"},
        {"skew", "Pfadversatz", "Pfad R 150 µs später als P (ST 2022-7)"},
        {"audio", "Ton-Jitter", "Tonpakete mit 600 µs Streuung: TSDF steigt, Urteil wide"},
        {"ptp", "PTP unruhig", "Korrekturen von mehreren µs: PTP nicht eingerastet"},
    };
    return kPresets;
}

DemoSource::DemoSource(const Options& options, Monitor& monitor)
    : options_(options), monitor_(monitor),
      videoP_(options.video ? options.video->format : VideoFormat{},
              options.audio ? options.audio->format : AudioFormat{}, 11),
      videoR_(options.video ? options.video->format : VideoFormat{},
              options.audio ? options.audio->format : AudioFormat{}, 12),
      audioP_(options.video ? options.video->format : VideoFormat{},
              options.audio ? options.audio->format : AudioFormat{}, 13),
      audioR_(options.video ? options.video->format : VideoFormat{},
              options.audio ? options.audio->format : AudioFormat{}, 14) {
    if (options_.video) {
        monitor_.setVideoPacketsPerFrame(videoP_.videoParams().packetsPerFrame);
    }
    monitor_.addEvent("info", "Demo gestartet: synthetische Ströme ohne Karte");
    thread_ = std::thread([this] { run(); });
}

DemoSource::~DemoSource() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool DemoSource::setPreset(const std::string& id) {
    SenderFaults f;
    double ptpNoise = 25;
    if (id == "jitter") {
        f.jitterNs = 1500;
    } else if (id == "burst") {
        f.burstPackets = 8;
    } else if (id == "late") {
        f.startOffsetNs = 40000;
    } else if (id == "loss") {
        f.lossRate = 0.0005;
    } else if (id == "skew") {
        f.pathSkewNs = 150000;
    } else if (id == "audio") {
        f.audioJitterUs = 600;
    } else if (id == "ptp") {
        ptpNoise = 4000;
    } else if (id != "ideal") {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        preset_ = id;
        faults_ = f;
        ptpNoiseNs_ = ptpNoise;
    }
    for (const auto& p : presets()) {
        if (p.id == id) {
            monitor_.addEvent("info", "Demo: Störung „" + p.label + "“ – " + p.description);
        }
    }
    return true;
}

std::string DemoSource::preset() const {
    std::lock_guard lock(mutex_);
    return preset_;
}

void DemoSource::emitVideo(uint64_t frame, const SenderFaults& faults) {
    const int lostP = videoP_.videoFrame(frame, faults, 0.0, [&](uint32_t rtp, uint64_t ns, int idx) {
        monitor_.onVideoPacket(Path::Primary, rtp, ns, idx, true);
    });
    const int packets = videoP_.videoParams().packetsPerFrame;
    auto& p = videoCounters_.paths[0];
    p.packets += static_cast<uint64_t>(packets - lostP);
    p.bytes += static_cast<uint64_t>(packets - lostP) * 1262;
    p.lost += static_cast<uint64_t>(lostP);
    videoCounters_.lost += static_cast<uint64_t>(lostP);
    videoCounters_.received += static_cast<uint64_t>(packets);
    if (options_.video->redundant) {
        const int lostR =
            videoR_.videoFrame(frame, faults, faults.pathSkewNs, [&](uint32_t rtp, uint64_t ns, int idx) {
                monitor_.onVideoPacket(Path::Redundant, rtp, ns, idx, true);
            });
        auto& r = videoCounters_.paths[1];
        r.packets += static_cast<uint64_t>(packets - lostR);
        r.bytes += static_cast<uint64_t>(packets - lostR) * 1262;
        r.lost += static_cast<uint64_t>(lostR);
        videoCounters_.lost += static_cast<uint64_t>(lostR);
        videoCounters_.redundant += static_cast<uint64_t>(packets - lostR);
    } else {
        videoCounters_.unrecovered += static_cast<uint64_t>(lostP);
        videoCounters_.received -= static_cast<uint64_t>(lostP);
    }
}

void DemoSource::emitAudio(uint64_t packet, const SenderFaults& faults) {
    const uint64_t bytes = options_.audio->format.payloadBytes() + 54;
    if (audioP_.audioPacket(packet, faults, 0.0, [&](uint32_t rtp, uint64_t ns) {
            monitor_.onAudioPacket(Path::Primary, rtp, ns, true);
        })) {
        audioCounters_.paths[0].packets++;
        audioCounters_.paths[0].bytes += bytes;
        audioCounters_.received++;
    } else {
        audioCounters_.paths[0].lost++;
        audioCounters_.lost++;
    }
    if (options_.audio->redundant) {
        if (audioR_.audioPacket(packet, faults, faults.pathSkewNs, [&](uint32_t rtp, uint64_t ns) {
                monitor_.onAudioPacket(Path::Redundant, rtp, ns, true);
            })) {
            audioCounters_.paths[1].packets++;
            audioCounters_.paths[1].bytes += bytes;
            audioCounters_.redundant++;
        } else {
            audioCounters_.paths[1].lost++;
            audioCounters_.lost++;
        }
    }
}

void DemoSource::run() {
    std::mt19937_64 rng(99);
    const double frameNs = options_.video ? framePeriodNs(options_.video->format.rate) : 0.0;
    const double audioNs = options_.audio ? packetTimeNs(options_.audio->format.ptime) : 0.0;
    // Erst ab dem nächsten vollen Zeitraum erzeugen.
    const uint64_t start = taiNowNs();
    uint64_t nextFrame = frameNs > 0 ? static_cast<uint64_t>(std::floor(start / frameNs)) : 0;
    uint64_t nextAudio = audioNs > 0 ? static_cast<uint64_t>(std::floor(start / audioNs)) : 0;
    auto nextPtp = std::chrono::steady_clock::now();
    auto nextCounters = nextPtp;

    while (!stop_) {
        SenderFaults faults;
        double ptpNoise;
        {
            std::lock_guard lock(mutex_);
            faults = faults_;
            ptpNoise = ptpNoiseNs_;
        }
        // Nur Zeiträume erzeugen, die vollständig vergangen sind.
        const uint64_t now = taiNowNs();
        if (frameNs > 0) {
            const auto last = static_cast<uint64_t>(std::floor(now / frameNs));
            for (; nextFrame + 1 < last && !stop_; ++nextFrame) {
                emitVideo(nextFrame, faults);
            }
        }
        if (audioNs > 0) {
            const auto last = static_cast<uint64_t>(std::floor(now / audioNs));
            for (; nextAudio + 2 < last && !stop_; ++nextAudio) {
                emitAudio(nextAudio, faults);
            }
        }
        const auto steady = std::chrono::steady_clock::now();
        if (steady >= nextPtp) {
            std::normal_distribution<double> noise(0.0, ptpNoise);
            monitor_.onPtpSync(static_cast<int64_t>(noise(rng)), 37);
            nextPtp += std::chrono::milliseconds(125);
        }
        if (steady >= nextCounters) {
            if (options_.video) {
                monitor_.setCounters(Media::Video, videoCounters_);
            }
            if (options_.audio) {
                monitor_.setCounters(Media::Audio, audioCounters_);
            }
            nextCounters += std::chrono::milliseconds(500);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace st2110
