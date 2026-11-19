#include "core/AudioPacketAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace st2110 {

AudioPassCriteria computeAudioPassCriteria(PacketTime ptime) {
    const double us = packetTimeNs(ptime) / 1000.0;
    AudioPassCriteria p;
    p.dpvrMaxNarrowUs = static_cast<int32_t>((1 + 1 + 1) * us);
    p.dpvrMaxWideUs = static_cast<int32_t>((1 + 1 + 17) * us);
    p.tsdfMaxNarrowUs = static_cast<int32_t>(1 * us);
    p.tsdfMaxWideUs = static_cast<int32_t>(17 * us);
    return p;
}

Compliance evaluateAudioPeriod(const AudioTiming& t, const AudioPassCriteria& p, std::string& cause) {
    auto failed = [&](const char* text) {
        cause = text;
        return Compliance::Failed;
    };
    if (t.dpvrMinUs < 0) {
        return failed("dpvr exceed min");
    }
    if (t.dpvrMaxUs > p.dpvrMaxWideUs) {
        return failed("dpvr exceed max wide");
    }
    if (t.tsdfUs < 0) {
        return failed("tsdf exceed min");
    }
    if (t.tsdfUs > p.tsdfMaxWideUs) {
        return failed("tsdf exceed max wide");
    }
    if (t.dpvrMaxUs > p.dpvrMaxNarrowUs) {
        cause = "dpvr exceed max narrow";
        return Compliance::Wide;
    }
    if (t.tsdfUs > p.tsdfMaxNarrowUs) {
        cause = "tsdf exceed max narrow";
        return Compliance::Wide;
    }
    cause = "narrow";
    return Compliance::Narrow;
}

void AudioStats::add(const AudioTiming& t) {
    ++periods_;
    packets_ += t.packets;
    counts_[static_cast<size_t>(t.compliance)]++;
    dpvr_.add(t.dpvrMinUs, t.dpvrMaxUs, t.dpvrAvgUs);
    ipt_.add(t.iptMinNs, t.iptMaxNs, t.iptAvgNs);
    tsdf_.add(t.tsdfUs);
    if (t.compliance != Compliance::Narrow && !t.failedCause.empty() && causes_.size() < 16) {
        causes_[t.failedCause]++;
    }
}

void AudioStats::merge(const AudioStats& o) {
    periods_ += o.periods_;
    packets_ += o.packets_;
    for (size_t i = 0; i < counts_.size(); ++i) {
        counts_[i] += o.counts_[i];
    }
    dpvr_.merge(o.dpvr_);
    ipt_.merge(o.ipt_);
    tsdf_.merge(o.tsdf_);
    for (const auto& [cause, n] : o.causes_) {
        if (causes_.count(cause) || causes_.size() < 16) {
            causes_[cause] += n;
        }
    }
}

Compliance AudioStats::worst() const {
    if (periods_ == 0 || count(Compliance::Failed) > 0) {
        return Compliance::Failed;
    }
    return count(Compliance::Wide) > 0 ? Compliance::Wide : Compliance::Narrow;
}

void AudioPacketAnalyzer::Histograms::clear() {
    dpvrUs.clear();
    iptNs.clear();
}

void AudioPacketAnalyzer::Histograms::merge(const Histograms& other) {
    dpvrUs.merge(other.dpvrUs);
    iptNs.merge(other.iptNs);
}

AudioPacketAnalyzer::AudioPacketAnalyzer(const AudioFormat& format)
    : format_(format), packetNs_(packetTimeNs(format.ptime)),
      samplesPerPacket_(static_cast<double>(format.samplesPerPacket())),
      pass_(computeAudioPassCriteria(format.ptime)) {
    const double packetUs = packetNs_ / 1000.0;
    Histograms h;
    // DPVR: −1 bis +21 Paketzeiten (Grenze wide: 19), 440 Klassen.
    h.dpvrUs = Histogram(-packetUs, 21.0 * packetUs, 440);
    // IPT: 0 bis 4 Paketzeiten, 400 Klassen.
    h.iptNs = Histogram(0.0, 4.0 * packetNs_, 400);
    for (auto& s : paths_) {
        s.total = h;
        s.interval = h;
    }
}

double AudioPacketAnalyzer::dpvrUs(uint32_t rtpTimestamp, uint64_t arrivalNs) const {
    const long double t = static_cast<long double>(arrivalNs);
    const long double epoch = std::floor(t / packetNs_);
    const long double fptDelta = t - epoch * packetNs_;
    const auto expectedRtp = static_cast<uint32_t>(static_cast<uint64_t>(epoch * samplesPerPacket_));
    const auto diff = static_cast<int32_t>(rtpTimestamp - expectedRtp);
    const long double latency = fptDelta - static_cast<long double>(diff) * packetNs_ / samplesPerPacket_;
    return static_cast<double>(latency / 1000.0L);
}

std::optional<AudioTiming> AudioPacketAnalyzer::finishPeriod(PathState& s) {
    if (!s.inPeriod || s.current.packets == 0) {
        s.inPeriod = false;
        return std::nullopt;
    }
    s.inPeriod = false;
    AudioTiming t = s.current;
    t.dpvrAvgUs = static_cast<float>(s.dpvrSum / t.packets);
    if (s.iptCount > 0) {
        t.iptAvgNs = static_cast<float>(s.iptSum / s.iptCount);
    } else {
        t.iptMinNs = t.iptMaxNs = 0;
    }
    t.tsdfUs = t.dpvrMaxUs - t.dpvrMinUs;
    t.compliance = evaluateAudioPeriod(t, pass_, t.failedCause);
    return t;
}

std::optional<AudioTiming> AudioPacketAnalyzer::onPacket(Path path, uint32_t rtpTimestamp,
                                                         uint64_t arrivalNs) {
    PathState& s = paths_[pathIndex(path)];
    std::optional<AudioTiming> finished;
    if (s.inPeriod && static_cast<double>(arrivalNs - s.periodStartNs) >= kPeriodNs) {
        finished = finishPeriod(s);
    }
    if (!s.inPeriod) {
        s.inPeriod = true;
        s.periodStartNs = arrivalNs;
        s.current = AudioTiming{};
        s.current.dpvrMinUs = s.current.iptMinNs = std::numeric_limits<int32_t>::max();
        s.current.dpvrMaxUs = s.current.iptMaxNs = std::numeric_limits<int32_t>::min();
        s.dpvrSum = s.iptSum = 0;
        s.iptCount = 0;
    }

    const double dpvr = dpvrUs(rtpTimestamp, arrivalNs);
    const auto dpvr32 = static_cast<int32_t>(dpvr);
    AudioTiming& t = s.current;
    t.packets++;
    t.dpvrMinUs = std::min(t.dpvrMinUs, dpvr32);
    t.dpvrMaxUs = std::max(t.dpvrMaxUs, dpvr32);
    s.dpvrSum += dpvr;
    s.total.dpvrUs.add(dpvr);
    s.interval.dpvrUs.add(dpvr);

    if (s.hasPrev && arrivalNs >= s.prevArrivalNs) {
        const auto ipt = static_cast<int64_t>(arrivalNs - s.prevArrivalNs);
        const auto ipt32 = static_cast<int32_t>(std::min<int64_t>(ipt, std::numeric_limits<int32_t>::max()));
        t.iptMinNs = std::min(t.iptMinNs, ipt32);
        t.iptMaxNs = std::max(t.iptMaxNs, ipt32);
        s.iptSum += static_cast<double>(ipt);
        s.iptCount++;
        s.total.iptNs.add(static_cast<double>(ipt));
        s.interval.iptNs.add(static_cast<double>(ipt));
    }
    s.prevArrivalNs = arrivalNs;
    s.hasPrev = true;
    return finished;
}

std::optional<AudioTiming> AudioPacketAnalyzer::flush(Path path) {
    return finishPeriod(paths_[pathIndex(path)]);
}

AudioPacketAnalyzer::Histograms AudioPacketAnalyzer::takeInterval(Path path) {
    PathState& s = paths_[pathIndex(path)];
    Histograms out = s.interval;
    s.interval.clear();
    return out;
}

void AudioPacketAnalyzer::resetTotals() {
    for (auto& s : paths_) {
        s.total.clear();
    }
}

} // namespace st2110
