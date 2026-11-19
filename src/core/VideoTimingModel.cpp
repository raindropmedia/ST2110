#include "core/VideoTimingModel.h"

#include <algorithm>
#include <cmath>

namespace st2110 {
namespace {

// RTP-Takt für Video: 90 kHz.
constexpr double kVideoClockHz = 90000.0;

} // namespace

VideoTimingParams computeVideoTimingParams(uint32_t height, bool interlaced, FrameRate rate,
                                           int packetsPerFrame) {
    VideoTimingParams p;
    p.frameTimeNs = framePeriodNs(rate);
    p.packetsPerFrame = packetsPerFrame;
    const double frameTimeSec = p.frameTimeNs / 1e9;

    p.rActive = 1080.0 / 1125.0;
    if (interlaced && height <= 576) {
        p.rActive = (height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
    }
    p.trsNs = packetsPerFrame > 0 ? p.frameTimeNs * p.rActive / packetsPerFrame : 0.0;

    if (!interlaced) {
        p.trOffsetNs = height >= 1080 ? p.frameTimeNs * (43.0 / 1125.0) : p.frameTimeNs * (28.0 / 750.0);
    } else if (height == 480) {
        p.trOffsetNs = p.frameTimeNs * (20.0 / 525.0) * 2;
    } else if (height == 576) {
        p.trOffsetNs = p.frameTimeNs * (26.0 / 625.0) * 2;
    } else {
        p.trOffsetNs = p.frameTimeNs * (22.0 / 1125.0) * 2;
    }

    const double n = packetsPerFrame;
    PassCriteria& c = p.pass;
    c.cinstMaxNarrow = static_cast<int32_t>(std::max(4.0, n / (43200.0 * p.rActive * frameTimeSec)));
    c.cinstMaxWide = static_cast<int32_t>(std::max(16.0, n / (21600.0 * frameTimeSec)));
    c.vrxMaxNarrow = static_cast<int32_t>(std::max(8.0, n / (27000.0 * frameTimeSec)));
    c.vrxMaxWide = static_cast<int32_t>(std::max(720.0, n / (300.0 * frameTimeSec)));
    c.trOffsetNs = static_cast<int32_t>(p.trOffsetNs);
    c.latencyMaxNs = 1000 * 1000;
    c.latencyMinNs = 0;
    c.rtpOffsetMax = static_cast<int32_t>(std::ceil(p.trOffsetNs * kVideoClockHz / 1e9)) + 1;
    c.rtpOffsetMin = -1;
    // Wie MTL abgeschnitten: 59,94 Hz ergibt 1501 (Abstände wechseln zwischen 1501 und 1502).
    const auto sampling = static_cast<int32_t>(std::floor(p.frameTimeNs * kVideoClockHz / 1e9 + 1e-6));
    c.rtpTsDeltaMax = sampling + 1;
    c.rtpTsDeltaMin = sampling;
    return p;
}

Compliance evaluateVideoFrame(const FrameTiming& f, const PassCriteria& c, std::string& cause) {
    auto failed = [&](const char* text) {
        cause = text;
        return Compliance::Failed;
    };
    if (f.fptNs > c.trOffsetNs) {
        return failed("fpt exceed tr_offset");
    }
    if (f.rtpTsDelta != 0 && f.rtpTsDelta < c.rtpTsDeltaMin) {
        return failed("rtp_ts_delta exceed min");
    }
    if (f.rtpTsDelta > c.rtpTsDeltaMax) {
        return failed("rtp_ts_delta exceed max");
    }
    if (f.rtpOffset < c.rtpOffsetMin) {
        return failed("rtp_offset exceed min");
    }
    if (f.rtpOffset > c.rtpOffsetMax) {
        return failed("rtp_offset exceed max");
    }
    if (f.latencyNs < c.latencyMinNs) {
        return failed("latency exceed min");
    }
    if (f.latencyNs > c.latencyMaxNs) {
        return failed("latency exceed max");
    }
    if (f.vrxMin < 0) {
        return failed("vrx exceed min");
    }
    if (f.vrxMax > c.vrxMaxWide) {
        return failed("vrx exceed max");
    }
    if (f.cinstMin > 0) {
        return failed("cinst exceed min");
    }
    if (f.cinstMax > c.cinstMaxWide) {
        return failed("cinst exceed max");
    }
    if (f.cinstMax > c.cinstMaxNarrow) {
        cause = "wide as cinst exceed narrow max";
        return Compliance::Wide;
    }
    if (f.vrxMax > c.vrxMaxNarrow) {
        cause = "wide as vrx exceed narrow max";
        return Compliance::Wide;
    }
    cause = "narrow";
    return Compliance::Narrow;
}

} // namespace st2110
