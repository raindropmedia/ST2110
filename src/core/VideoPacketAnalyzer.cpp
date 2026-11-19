#include "core/VideoPacketAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace st2110 {
namespace {

constexpr double kVideoClockHz = 90000.0;

int16_t clamp16(long value) {
    return static_cast<int16_t>(
        std::clamp<long>(value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

int32_t clamp32(long double value) {
    if (std::isnan(static_cast<double>(value))) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp<long double>(value, std::numeric_limits<int32_t>::min(),
                                                        std::numeric_limits<int32_t>::max()));
}

} // namespace

void PacketProfile::clear() {
    rtpTimestamp = 0;
    index.clear();
    arrivalUs.clear();
    cinst.clear();
    vrx.clear();
    timing = FrameTiming{};
}

void VideoPacketAnalyzer::Histograms::clear() {
    iptNs.clear();
    cinst.clear();
    vrx.clear();
}

void VideoPacketAnalyzer::Histograms::merge(const Histograms& other) {
    iptNs.merge(other.iptNs);
    cinst.merge(other.cinst);
    vrx.merge(other.vrx);
}

VideoPacketAnalyzer::VideoPacketAnalyzer(uint32_t height, bool interlaced, FrameRate rate)
    : height_(height), interlaced_(interlaced), rate_(rate), frameTimeNs_(framePeriodNs(rate)),
      rtpTicksPerFrame_(frameTimeNs_ * kVideoClockHz / 1e9) {}

void VideoPacketAnalyzer::setPacketsPerFrame(int packets) {
    if (packets <= 0 || (params_ && params_->packetsPerFrame == packets)) {
        return;
    }
    params_ = computeVideoTimingParams(height_, interlaced_, rate_, packets);
    // Neue Einteilung: Histogramme neu anlegen.
    for (auto& state : paths_) {
        state.total = Histograms{};
        state.interval = Histograms{};
        ensureHistograms(state);
    }
}

void VideoPacketAnalyzer::ensureHistograms(PathState& state) {
    if (!params_ || state.total.iptNs.configured()) {
        return;
    }
    const auto& p = *params_;
    Histograms h;
    // IPT: 0 bis 8 TRS in 800 Klassen (bei 1080p50 rund 44 ns je Klasse; Bündel bis 8 Pakete sichtbar).
    h.iptNs = Histogram(0.0, 8.0 * p.trsNs, 800);
    // Cinst und VRX ganzzahlig, eine Klasse je Wert.
    const int cinstUpper = std::max(p.pass.cinstMaxWide, p.pass.cinstMaxNarrow * 2) + 8;
    h.cinst = Histogram(0.0, cinstUpper, static_cast<size_t>(cinstUpper));
    const int vrxUpper = std::max(64, p.pass.vrxMaxNarrow * 8);
    h.vrx = Histogram(-32.0, vrxUpper, static_cast<size_t>(vrxUpper + 32));
    state.total = h;
    state.interval = h;
}

void VideoPacketAnalyzer::learnPacketCount(int maxIndex, int previousMaxIndex) {
    if (params_ || maxIndex <= 0) {
        return;
    }
    if (maxIndex == previousMaxIndex) {
        setPacketsPerFrame(maxIndex + 1);
    }
}

void VideoPacketAnalyzer::startFrame(PathState& s, uint32_t rtpTimestamp, uint64_t arrivalNs,
                                     int packetIndex) {
    s.inFrame = true;
    s.rtp = rtpTimestamp;
    s.maxIndex = packetIndex;
    s.frame = FrameTiming{};
    s.frame.cinstMin = std::numeric_limits<int32_t>::max();
    s.frame.vrxMin = std::numeric_limits<int32_t>::max();
    s.frame.iptMinNs = std::numeric_limits<int32_t>::max();
    s.frame.cinstMax = s.frame.vrxMax = s.frame.iptMaxNs = std::numeric_limits<int32_t>::min();
    s.cinstSum = s.vrxSum = s.iptSum = 0;
    s.iptCount = 0;
    s.hasPrevArrival = false;
    s.current.clear();
    s.current.rtpTimestamp = rtpTimestamp;

    const long double t = static_cast<long double>(arrivalNs);
    const long double epoch = std::floor(t / frameTimeNs_);
    s.epochStartNs = epoch * frameTimeNs_;
    s.frameHasParams = params_.has_value();
    if (!params_) {
        return;
    }
    const auto& p = *params_;
    s.firstPacketNs = t - static_cast<long double>(p.trsNs) * packetIndex;
    const long double fpt = s.firstPacketNs - s.epochStartNs;

    // RTP-Soll der Epoche (32 Bit) und Abweichung mit Überlauf-sicherer Differenz.
    const auto expectedRtp = static_cast<uint32_t>(static_cast<uint64_t>(epoch * rtpTicksPerFrame_));
    const auto rtpOffset = static_cast<int32_t>(rtpTimestamp - expectedRtp);
    s.frame.fptNs = clamp32(fpt);
    s.frame.rtpOffset = rtpOffset;
    s.frame.latencyNs = clamp32(fpt - static_cast<long double>(rtpOffset) * frameTimeNs_ / rtpTicksPerFrame_);
    s.frame.rtpTsDelta = s.hasPrevRtp ? static_cast<int32_t>(rtpTimestamp - s.prevRtp) : 0;
}

std::optional<FrameTiming> VideoPacketAnalyzer::finishFrame(PathState& s) {
    if (!s.inFrame) {
        return std::nullopt;
    }
    s.inFrame = false;
    s.hasPrevRtp = true;
    s.prevRtp = s.rtp;
    const int previousMax = s.lastFrameMaxIndex;
    s.lastFrameMaxIndex = s.maxIndex;
    if (!params_) {
        learnPacketCount(s.maxIndex, previousMax);
        return std::nullopt;
    }

    FrameTiming& f = s.frame;
    if (f.packets == 0) {
        return std::nullopt;
    }
    f.cinstAvg = static_cast<float>(s.cinstSum) / static_cast<float>(f.packets);
    f.vrxAvg = static_cast<float>(s.vrxSum) / static_cast<float>(f.packets);
    if (s.iptCount > 0) {
        f.iptAvgNs = static_cast<float>(s.iptSum) / static_cast<float>(s.iptCount);
    } else {
        f.iptMinNs = f.iptMaxNs = 0;
    }
    f.compliance = evaluateVideoFrame(f, params_->pass, f.failedCause);

    s.current.timing = f;
    // Schlechtestes Bild: höchstes Cinst, bei Gleichstand kleinstes VRX.
    const bool worse = s.worst.empty() || f.cinstMax > s.worst.timing.cinstMax ||
                       (f.cinstMax == s.worst.timing.cinstMax && f.vrxMin < s.worst.timing.vrxMin);
    if (worse) {
        s.worst = s.current;
    }
    std::swap(s.last, s.current);
    s.current.clear();
    return f;
}

std::optional<FrameTiming> VideoPacketAnalyzer::onPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs,
                                                         int packetIndex) {
    PathState& s = paths_[pathIndex(path)];
    std::optional<FrameTiming> finished;

    if (!s.inFrame || rtpTimestamp != s.rtp) {
        if (s.hasPrevRtp && rtpTimestamp == s.prevRtp) {
            return std::nullopt; // verspätetes Paket des vorherigen Bildes
        }
        finished = finishFrame(s);
        ensureHistograms(s);
        startFrame(s, rtpTimestamp, arrivalNs, packetIndex);
    }
    s.maxIndex = std::max(s.maxIndex, packetIndex);
    if (!params_ || !s.frameHasParams) {
        return finished;
    }

    const auto& p = *params_;
    const long double t = static_cast<long double>(arrivalNs);
    const long double expected = s.epochStartNs + p.trOffsetNs + p.trsNs * (packetIndex + 1);
    const auto vrx = static_cast<long>((expected - t) / p.trsNs);
    const auto drained = static_cast<long>(((t - s.firstPacketNs) / p.trsNs) * kCinstDrainFactor);
    const long cinst = std::max(0L, static_cast<long>(packetIndex) - drained);

    FrameTiming& f = s.frame;
    f.packets++;
    f.cinstMin = std::min<int32_t>(f.cinstMin, static_cast<int32_t>(cinst));
    f.cinstMax = std::max<int32_t>(f.cinstMax, static_cast<int32_t>(cinst));
    f.vrxMin = std::min<int32_t>(f.vrxMin, static_cast<int32_t>(vrx));
    f.vrxMax = std::max<int32_t>(f.vrxMax, static_cast<int32_t>(vrx));
    s.cinstSum += cinst;
    s.vrxSum += vrx;
    s.total.cinst.add(static_cast<double>(cinst));
    s.interval.cinst.add(static_cast<double>(cinst));
    s.total.vrx.add(static_cast<double>(vrx));
    s.interval.vrx.add(static_cast<double>(vrx));

    if (s.hasPrevArrival && arrivalNs >= s.prevArrivalNs) {
        const auto ipt = static_cast<int64_t>(arrivalNs - s.prevArrivalNs);
        const auto ipt32 = static_cast<int32_t>(std::min<int64_t>(ipt, std::numeric_limits<int32_t>::max()));
        f.iptMinNs = std::min(f.iptMinNs, ipt32);
        f.iptMaxNs = std::max(f.iptMaxNs, ipt32);
        s.iptSum += ipt;
        s.iptCount++;
        s.total.iptNs.add(static_cast<double>(ipt));
        s.interval.iptNs.add(static_cast<double>(ipt));
    }
    s.prevArrivalNs = arrivalNs;
    s.hasPrevArrival = true;

    if (s.current.index.size() < kMaxProfilePackets) {
        s.current.index.push_back(packetIndex);
        s.current.arrivalUs.push_back(static_cast<float>((t - s.epochStartNs) / 1000.0L));
        s.current.cinst.push_back(clamp16(cinst));
        s.current.vrx.push_back(clamp16(vrx));
    }
    return finished;
}

std::optional<FrameTiming> VideoPacketAnalyzer::flush(Path path) {
    return finishFrame(paths_[pathIndex(path)]);
}

VideoPacketAnalyzer::Histograms VideoPacketAnalyzer::takeInterval(Path path) {
    PathState& s = paths_[pathIndex(path)];
    Histograms out = s.interval;
    s.interval.clear();
    return out;
}

void VideoPacketAnalyzer::resetTotals() {
    for (auto& s : paths_) {
        s.total.clear();
        s.worst.clear();
    }
}

} // namespace st2110
