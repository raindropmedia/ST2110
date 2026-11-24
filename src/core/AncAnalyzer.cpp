#include "core/AncAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st2110 {

using nlohmann::json;

namespace {

constexpr double kRtpClockHz = 90000.0;
constexpr double kVerdictWindowSec = 10.0;
constexpr size_t kSeriesCapacity = 6000;

json check(const char* id, const char* label, const std::string& value, const std::string& limit,
           const char* verdict, const std::string& note = {}) {
    return json{{"id", id},       {"label", label},     {"value", value},
                {"limit", limit}, {"verdict", verdict}, {"note", note}};
}

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

const char* levelName(AncAnalyzer::Level level) {
    switch (level) {
    case AncAnalyzer::Level::Ok:
        return "ok";
    case AncAnalyzer::Level::Warn:
        return "warn";
    case AncAnalyzer::Level::Fail:
        break;
    }
    return "failed";
}

} // namespace

const char* ancDataName(uint8_t did, uint8_t sdid) {
    switch (did) {
    case 0x41:
        switch (sdid) {
        case 0x01:
            return "Payload Identification (ST 352)";
        case 0x05:
            return "Active Format Description (AFD)";
        case 0x06:
            return "Bar Data";
        default:
            return "";
        }
    case 0x45:
        return sdid >= 0x01 && sdid <= 0x03 ? "Audio-Metadaten (ST 2020)" : "";
    case 0x60:
        return sdid == 0x60 ? "Zeitcode ATC (ST 12-2)" : "";
    case 0x61:
        switch (sdid) {
        case 0x01:
            return "Untertitel CEA-708 (CDP)";
        case 0x02:
            return "Untertitel CEA-608";
        default:
            return "";
        }
    case 0x43:
        switch (sdid) {
        case 0x02:
            return "Teletext OP-47 (SDP)";
        case 0x03:
            return "Teletext OP-47 (Multipacket)";
        default:
            return "";
        }
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
        return "SDI-Audio-Steuerpaket (ST 299-1)";
    case 0xE4:
    case 0xE5:
    case 0xE6:
    case 0xE7:
        return "SDI-Audio-Daten (ST 299-1)";
    default:
        return "";
    }
}

AncAnalyzer::AncAnalyzer(const VideoFormat& format)
    : rate_(format.rate), interlaced_(format.interlaced), periodNs_(framePeriodNs(format.rate)),
      perFrameSeries_(kSeriesCapacity), latencySeries_(kSeriesCapacity) {
    const double ticks = periodNs_ * kRtpClockHz / 1e9;
    expectedStepMin_ = static_cast<int>(std::floor(ticks));
    expectedStepMax_ = static_cast<int>(std::ceil(ticks));
    // 10-µs-Klassen von −1 ms bis +4 ms: Der erste ANC-Datenstrom liegt meist kurz hinter der Sollzeit.
    latencyHist_ = Histogram(-1000.0, 4000.0, 500);
}

void AncAnalyzer::reset() {
    *this = AncAnalyzer(VideoFormat{1920, 1080, rate_, interlaced_, PixelFormat{}});
}

AncAnalyzer::FrameResult AncAnalyzer::add(const AncFrameReport& f, double nowSec) {
    FrameResult result;
    auto raise = [&](Level level, const std::string& cause) {
        if (level > result.level || (level == result.level && result.cause.empty())) {
            result.level = level;
            result.cause = cause;
        }
    };

    ++frames_;
    rtpPackets_ += f.packets;
    seqLost_ += f.seqLost;
    if (f.seqDiscont) {
        ++discont_;
    }
    if (f.corrupted) {
        ++corrupted_;
    }
    if (!f.marker) {
        ++noMarker_;
    }
    if (f.anc.empty()) {
        ++empty_;
    }

    // Zeitstempelschritt gegen die Bildperiode (bei Zeilensprung je Halbbild)
    if (haveLastTs_) {
        const int32_t step = static_cast<int32_t>(f.rtpTimestamp - lastTs_);
        lastStep_ = step;
        if (!haveStep_) {
            minStep_ = maxStep_ = step;
            haveStep_ = true;
        } else {
            minStep_ = std::min(minStep_, step);
            maxStep_ = std::max(maxStep_, step);
        }
        if (step < expectedStepMin_ || step > expectedStepMax_) {
            ++stepErrors_;
            raise(Level::Warn,
                  "RTP-Zeitstempelschritt " + std::to_string(step) + " statt " +
                      std::to_string(expectedStepMin_) +
                      (expectedStepMax_ != expectedStepMin_ ? "/" + std::to_string(expectedStepMax_) : ""));
        }
    }
    haveLastTs_ = true;
    lastTs_ = f.rtpTimestamp;

    // Zeitbezug: Ankunft des ersten Pakets gegen die Sollzeit aus dem RTP-Zeitstempel
    if (f.firstPacketNs != 0) {
        const uint64_t seconds = f.firstPacketNs / 1000000000ULL;
        const uint64_t rest = f.firstPacketNs % 1000000000ULL;
        const uint32_t ticks = static_cast<uint32_t>(seconds * 90000ULL + rest * 90000ULL / 1000000000ULL);
        const int32_t delta = static_cast<int32_t>(ticks - f.rtpTimestamp);
        const int64_t latencyNs = static_cast<int64_t>(std::llround(delta * 1e9 / kRtpClockHz));
        lastLatencyNs_ = latencyNs;
        const double us = static_cast<double>(latencyNs) / 1000.0;
        if (latencyCount_ == 0) {
            latencyMinUs_ = latencyMaxUs_ = us;
        } else {
            latencyMinUs_ = std::min(latencyMinUs_, us);
            latencyMaxUs_ = std::max(latencyMaxUs_, us);
        }
        latencySumUs_ += us;
        ++latencyCount_;
        latencyHist_.add(us);
        latencySeries_.add(nowSec, us);
        if (std::abs(static_cast<double>(latencyNs)) > periodNs_) {
            ++latencyWarnings_;
            raise(Level::Warn, "Zeitbezug: Ankunft " + fixed(us / 1000.0, 1) +
                                   " ms neben der Sollzeit (mehr als eine Bildperiode)");
        }
    }

    if (!f.marker) {
        raise(Level::Warn, "kein RTP-Marker im Bild");
    }
    if (f.seqLost > 0 || f.seqDiscont) {
        raise(Level::Fail, std::to_string(f.seqLost) + " RTP-Pakete fehlen");
    }
    if (f.corrupted) {
        raise(Level::Fail, "Bild beschädigt (MTL: corrupted)");
    }

    // Zählung je DID/SDID
    std::map<std::pair<uint8_t, uint8_t>, uint32_t> inFrame;
    for (const auto& a : f.anc) {
        auto& s = data_[{a.did, a.sdid}];
        ++s.count;
        s.udwWords += a.udwSize;
        s.minLine = std::min(s.minLine, a.line);
        s.maxLine = std::max(s.maxLine, a.line);
        s.lastLine = a.line;
        s.lastHori = a.horiOffset;
        s.lastUdw = a.udwSize;
        s.luma = a.luma;
        s.lastSeenSec = nowSec;
        ++inFrame[{a.did, a.sdid}];
        udwWords_ += a.udwSize;
    }
    for (const auto& [key, count] : inFrame) {
        auto& s = data_[key];
        ++s.framesWith;
        s.maxPerFrame = std::max(s.maxPerFrame, count);
    }
    const uint32_t n = static_cast<uint32_t>(f.anc.size());
    ancPackets_ += n;
    perFrameMin_ = std::min(perFrameMin_, n);
    perFrameMax_ = std::max(perFrameMax_, n);
    perFrameSum_ += n;
    perFrameSeries_.add(nowSec, n);
    last_ = f;

    if (result.level == Level::Fail) {
        lastFailSec_ = nowSec;
        lastCause_ = result.cause;
    } else if (result.level == Level::Warn) {
        lastWarnSec_ = nowSec;
        lastCause_ = result.cause;
    }
    return result;
}

AncAnalyzer::Level AncAnalyzer::verdict(double nowSec) const {
    if (nowSec - lastFailSec_ < kVerdictWindowSec) {
        return Level::Fail;
    }
    if (nowSec - lastWarnSec_ < kVerdictWindowSec) {
        return Level::Warn;
    }
    return Level::Ok;
}

json AncAnalyzer::toJson(double nowSec, double windowSec, size_t maxPoints) const {
    json data = json::array();
    std::vector<std::pair<std::pair<uint8_t, uint8_t>, const DataStats*>> sorted;
    for (const auto& [key, stats] : data_) {
        sorted.emplace_back(key, &stats);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
    for (const auto& [key, s] : sorted) {
        data.push_back(json{{"did", key.first},
                            {"sdid", key.second},
                            {"name", ancDataName(key.first, key.second)},
                            {"count", s->count},
                            {"udwWords", s->udwWords},
                            {"framesWith", s->framesWith},
                            {"maxPerFrame", s->maxPerFrame},
                            {"minLine", s->minLine},
                            {"maxLine", s->maxLine},
                            {"lastLine", s->lastLine},
                            {"lastHoriOffset", s->lastHori},
                            {"lastUdwWords", s->lastUdw},
                            {"luma", s->luma},
                            {"sinceLastSec", nowSec - s->lastSeenSec}});
    }
    json last = nullptr;
    if (frames_ > 0) {
        json packets = json::array();
        for (const auto& a : last_.anc) {
            packets.push_back(json{{"did", a.did},
                                   {"sdid", a.sdid},
                                   {"name", ancDataName(a.did, a.sdid)},
                                   {"line", a.line},
                                   {"horiOffset", a.horiOffset},
                                   {"udwWords", a.udwSize},
                                   {"luma", a.luma},
                                   {"streamNum", a.streamNum}});
        }
        last = json{{"rtpTimestamp", last_.rtpTimestamp}, {"rtpPackets", last_.packets},
                    {"seqLost", last_.seqLost},           {"marker", last_.marker},
                    {"interlaced", last_.interlaced},     {"secondField", last_.secondField},
                    {"corrupted", last_.corrupted},       {"anc", packets}};
    }
    const double avgPerFrame =
        frames_ ? static_cast<double>(perFrameSum_) / static_cast<double>(frames_) : 0.0;
    return json{
        {"verdict", frames_ ? json(levelName(verdict(nowSec))) : json(nullptr)},
        {"cause", lastCause_},
        {"frames",
         json{{"total", frames_}, {"corrupted", corrupted_}, {"empty", empty_}, {"noMarker", noMarker_}}},
        {"rtpPackets", rtpPackets_},
        {"ancPackets", ancPackets_},
        {"udwWords", udwWords_},
        {"seqLost", seqLost_},
        {"seqDiscontFrames", discont_},
        {"perFrame", json{{"min", frames_ ? perFrameMin_ : 0u}, {"max", perFrameMax_}, {"avg", avgPerFrame}}},
        {"rtpStep", json{{"last", haveStep_ ? json(lastStep_) : json(nullptr)},
                         {"min", haveStep_ ? json(minStep_) : json(nullptr)},
                         {"max", haveStep_ ? json(maxStep_) : json(nullptr)},
                         {"expectedMin", expectedStepMin_},
                         {"expectedMax", expectedStepMax_},
                         {"errors", stepErrors_}}},
        {"latency", latencyCount_ ? json{{"lastUs", static_cast<double>(lastLatencyNs_) / 1000.0},
                                         {"minUs", latencyMinUs_},
                                         {"maxUs", latencyMaxUs_},
                                         {"avgUs", latencySumUs_ / static_cast<double>(latencyCount_)},
                                         {"warnings", latencyWarnings_},
                                         {"histogram", histogramToJson(latencyHist_)}}
                                  : json(nullptr)},
        {"periodUs", periodNs_ / 1000.0},
        {"data", data},
        {"last", last},
        {"series", json{{"ancPerFrame", perFrameSeries_.toJson(windowSec, maxPoints)},
                        {"latencyUs", latencySeries_.toJson(windowSec, maxPoints)}}},
        {"system10", system10Checks()},
    };
}

json AncAnalyzer::system10Checks() const {
    json checks = json::array();
    if (latencyCount_ > 0) {
        const double limit = periodNs_ / 1000.0;
        const bool inside = latencyMinUs_ >= -limit && latencyMaxUs_ <= limit;
        checks.push_back(check("rtp-ptp", "RTP-Zeitstempel gegen PTP-Zeit (erstes ANC-Paket)",
                               fixed(latencyMinUs_, 0) + " … " + fixed(latencyMaxUs_, 0) + " µs",
                               fixed(-limit, 0) + " … " + fixed(limit, 0) + " µs (eine Bildperiode)",
                               inside ? "ok" : "warn",
                               "Richtwert: ST 2110-40 nennt keinen Grenzwert; ANC-Pakete gehören zur "
                               "Bildperiode des RTP-Zeitstempels."));
    }
    if (haveStep_) {
        const bool ok = minStep_ >= expectedStepMin_ && maxStep_ <= expectedStepMax_;
        checks.push_back(
            check("rtp-step", "RTP-Zeitstempelschritt je Bild (90 kHz)",
                  std::to_string(minStep_) + " … " + std::to_string(maxStep_),
                  std::to_string(expectedStepMin_) +
                      (expectedStepMax_ != expectedStepMin_ ? " … " + std::to_string(expectedStepMax_) : ""),
                  ok ? "ok" : "fail", "Takt von 90 kHz: Schritt = 90000 / Bildrate."));
    }
    return checks;
}

} // namespace st2110
