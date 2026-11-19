#include "core/FlowMonitor.h"

#include "core/Report.h"

#include <algorithm>
#include <cmath>

namespace st2110 {

using nlohmann::json;

namespace {

constexpr size_t kSeriesCapacity = 12000; // 50 Hz x 240 s
constexpr size_t kMaxSeriesPoints = 1200;
constexpr size_t kMaxProfilePoints = 2000;

json flowToJson(const FlowAddress& f) {
    return flowAddressToJson(f);
}

const char* levelFor(Compliance c) {
    switch (c) {
    case Compliance::Narrow:
        return "info";
    case Compliance::Wide:
        return "warn";
    case Compliance::Failed:
        break;
    }
    return "error";
}

json frameTimingToJson(const FrameTiming& f) {
    return json{{"cinstMax", f.cinstMax},
                {"cinstAvg", f.cinstAvg},
                {"vrxMin", f.vrxMin},
                {"vrxMax", f.vrxMax},
                {"vrxAvg", f.vrxAvg},
                {"iptMinNs", f.iptMinNs},
                {"iptMaxNs", f.iptMaxNs},
                {"iptAvgNs", f.iptAvgNs},
                {"fptNs", f.fptNs},
                {"latencyNs", f.latencyNs},
                {"rtpOffset", f.rtpOffset},
                {"rtpTsDelta", f.rtpTsDelta},
                {"verdict", complianceName(f.compliance)},
                {"cause", f.failedCause},
                {"packets", f.packets}};
}

json audioTimingToJson(const AudioTiming& a) {
    return json{{"dpvrMinUs", a.dpvrMinUs}, {"dpvrMaxUs", a.dpvrMaxUs},
                {"dpvrAvgUs", a.dpvrAvgUs}, {"iptMinNs", a.iptMinNs},
                {"iptMaxNs", a.iptMaxNs},   {"iptAvgNs", a.iptAvgNs},
                {"tsdfUs", a.tsdfUs},       {"verdict", complianceName(a.compliance)},
                {"cause", a.failedCause},   {"packets", a.packets}};
}

json frameCountersToJson(const FrameCounters& f) {
    return json{{"total", f.total()},
                {"complete", f.complete},
                {"reconstructed", f.reconstructed},
                {"corrupted", f.corrupted},
                {"dropped", f.dropped}};
}

} // namespace

json flowAddressToJson(const FlowAddress& f) {
    json out{{"ip", f.ip}, {"udpPort", f.udpPort}};
    if (!f.sourceIp.empty()) {
        out["sourceIp"] = f.sourceIp;
    }
    return out;
}

FlowMonitor::FlowMonitor(FlowConfig config, EventSink sink, Clock clock)
    : config_(std::move(config)), sink_(std::move(sink)), clock_(std::move(clock)) {
    if (!clock_) {
        const auto start = std::chrono::steady_clock::now();
        clock_ = [start] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
    }
    startSec_ = clock_();
    if (config_.video) {
        const auto& f = config_.video->format;
        videoAnalyzer_ = std::make_unique<VideoPacketAnalyzer>(f.height, f.interlaced, f.rate);
    }
    if (config_.audio) {
        audioAnalyzer_ = std::make_unique<AudioPacketAnalyzer>(config_.audio->format);
        audioPreview_.configure(config_.audio->format.sampleRate, config_.audio->format.channels);
    }
    for (auto& p : video_) {
        for (TimeSeries* s :
             {&p.cinstMax, &p.vrxMin, &p.vrxMax, &p.fptUs, &p.latencyUs, &p.rtpOffset, &p.iptMaxNs}) {
            *s = TimeSeries(kSeriesCapacity);
        }
    }
}

void FlowMonitor::addEvent(const std::string& level, const std::string& text) {
    std::lock_guard lock(mutex_);
    addEventLocked(level, text);
}

void FlowMonitor::addEventLocked(const std::string& level, const std::string& text) {
    // Reihenfolge der Sperren: Flow vor Hub. Der Hub ruft Flows nie unter seiner Sperre auf.
    if (sink_) {
        sink_(config_.id, level, text);
    }
}

void FlowMonitor::onVideoPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, int packetIndex,
                                bool hw) {
    std::lock_guard lock(mutex_);
    ++videoPackets_;
    if (!hw) {
        ++videoSwPackets_;
    }
    if (!videoAnalyzer_) {
        return;
    }
    auto finished = videoAnalyzer_->onPacket(path, rtpTimestamp, arrivalNs, packetIndex);
    if (finished && !mtlVideoFrames_) {
        recordVideoFrame(path, *finished);
        if (path == Path::Primary) {
            framesInterval_.complete++;
            framesTotal_.complete++;
        }
        const uint32_t rtp = videoAnalyzer_->lastProfile(path).rtpTimestamp;
        videoSkew_.add(path == Path::Redundant, rtp, finished->fptNs, now() - startSec_);
    }
}

void FlowMonitor::onVideoFrame(const VideoFrameReport& report) {
    std::lock_guard lock(mutex_);
    mtlVideoFrames_ = true;
    FrameCounters one;
    switch (report.status) {
    case FrameStatus::Complete:
        one.complete = 1;
        break;
    case FrameStatus::Reconstructed:
        one.reconstructed = 1;
        break;
    case FrameStatus::Corrupted:
        one.corrupted = 1;
        addEventLocked("error", "Video: Bild beschädigt (Pakete fehlen, " +
                                    std::to_string(report.packetsTotal) + " empfangen)");
        break;
    case FrameStatus::Dropped:
        one.dropped = 1;
        break;
    }
    framesInterval_.merge(one);
    framesTotal_.merge(one);
    if (videoAnalyzer_ && report.packetsTotal > 0 && report.status == FrameStatus::Complete) {
        videoAnalyzer_->setPacketsPerFrame(static_cast<int>(report.packetsTotal));
    }
    const double t = now() - startSec_;
    for (size_t i = 0; i < kPathCount; ++i) {
        if (report.timing[i]) {
            const Path path = static_cast<Path>(i);
            recordVideoFrame(path, *report.timing[i]);
            videoSkew_.add(path == Path::Redundant, report.rtpTimestamp, report.timing[i]->fptNs, t);
        }
    }
}

void FlowMonitor::setVideoPassCriteria(const PassCriteria& criteria) {
    std::lock_guard lock(mutex_);
    mtlVideoPass_ = criteria;
}

void FlowMonitor::setVideoPacketsPerFrame(int packets) {
    std::lock_guard lock(mutex_);
    if (videoAnalyzer_) {
        videoAnalyzer_->setPacketsPerFrame(packets);
    }
}

void FlowMonitor::recordVideoFrame(Path path, const FrameTiming& f) {
    VideoPath& p = video_[pathIndex(path)];
    const double t = now() - startSec_;
    p.interval.add(f);
    p.total.add(f);
    p.lastFrame = f;
    p.cinstMax.add(t, f.cinstMax);
    p.vrxMin.add(t, f.vrxMin);
    p.vrxMax.add(t, f.vrxMax);
    p.fptUs.add(t, f.fptNs / 1000.0);
    p.latencyUs.add(t, f.latencyNs / 1000.0);
    p.rtpOffset.add(t, f.rtpOffset);
    p.iptMaxNs.add(t, f.iptMaxNs);

    if (p.secondStart < 0) {
        p.secondStart = t;
    }
    p.second.add(f);
    if (t - p.secondStart >= 1.0) {
        const Compliance verdict = p.second.worst();
        std::string cause;
        if (!p.second.failedCauses().empty()) {
            cause = " (" + p.second.failedCauses().begin()->first + ")";
        }
        const std::string name = std::string("Video ") + pathKey(path) + ": ";
        if (!p.lastSecondVerdict) {
            addEventLocked(levelFor(verdict), name + "erstes Urteil " + complianceName(verdict) + cause);
        } else if (*p.lastSecondVerdict != verdict) {
            addEventLocked(levelFor(verdict), name + complianceName(*p.lastSecondVerdict) + " → " +
                                                  complianceName(verdict) + cause);
        }
        p.lastSecondVerdict = verdict;
        p.second.reset();
        p.secondStart = t;
    }
}

void FlowMonitor::onAudioPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, bool hw) {
    std::lock_guard lock(mutex_);
    ++audioPackets_;
    if (!hw) {
        ++audioSwPackets_;
    }
    if (!audioAnalyzer_) {
        return;
    }
    auto finished = audioAnalyzer_->onPacket(path, rtpTimestamp, arrivalNs);
    if (finished && !mtlAudioPeriods_) {
        recordAudioPeriod(path, *finished);
    }
}

void FlowMonitor::onAudioPeriod(Path path, const AudioTiming& timing) {
    std::lock_guard lock(mutex_);
    mtlAudioPeriods_ = true;
    recordAudioPeriod(path, timing);
}

void FlowMonitor::recordAudioPeriod(Path path, const AudioTiming& a) {
    AudioPath& p = audio_[pathIndex(path)];
    const double t = now() - startSec_;
    p.interval.add(a);
    p.total.add(a);
    p.lastPeriod = a;
    p.dpvrMaxUs.add(t, a.dpvrMaxUs);
    p.dpvrMinUs.add(t, a.dpvrMinUs);
    p.tsdfUs.add(t, a.tsdfUs);
    p.iptMaxNs.add(t, a.iptMaxNs);
    const std::string name = std::string("Ton ") + pathKey(path) + ": ";
    if (!p.lastVerdict) {
        addEventLocked(levelFor(a.compliance), name + "erstes Urteil " + complianceName(a.compliance));
    } else if (*p.lastVerdict != a.compliance) {
        addEventLocked(levelFor(a.compliance), name + complianceName(*p.lastVerdict) + " → " +
                                                   complianceName(a.compliance) + " (" + a.failedCause + ")");
    }
    p.lastVerdict = a.compliance;
}

void FlowMonitor::setCounters(const StreamCounters& c) {
    std::lock_guard lock(mutex_);
    CounterState& s = counters_;
    const char* name = config_.video ? "Video" : "Ton";
    const double t = now();
    if (s.any) {
        for (size_t i = 0; i < kPathCount; ++i) {
            if (c.paths[i].lost > s.current.paths[i].lost) {
                addEventLocked("warn", std::string(name) + " " + pathKey(static_cast<Path>(i)) + ": " +
                                           std::to_string(c.paths[i].lost - s.current.paths[i].lost) +
                                           " Pakete verloren");
            }
        }
        if (c.unrecovered > s.current.unrecovered) {
            addEventLocked("error", std::string(name) + ": " +
                                        std::to_string(c.unrecovered - s.current.unrecovered) +
                                        " Pakete auch nach ST 2022-7 verloren");
        }
        // Raten aus der Änderung seit der letzten Berechnung (mindestens 0,5 s Abstand).
        const double dt = t - s.rateTime;
        if (dt >= 0.5) {
            for (size_t i = 0; i < kPathCount; ++i) {
                s.pps[i] = static_cast<double>(c.paths[i].packets - s.rateBase.paths[i].packets) / dt;
                s.bps[i] = static_cast<double>(c.paths[i].bytes - s.rateBase.paths[i].bytes) * 8.0 / dt;
            }
            s.rateBase = c;
            s.rateTime = t;
            s.ratesValid = true;
        }
    } else {
        s.lastInterval = c;
        s.rateBase = c;
        s.rateTime = t;
    }
    s.current = c;
    s.any = true;
}

void FlowMonitor::reset() {
    std::lock_guard lock(mutex_);
    for (auto& p : video_) {
        p.total.reset();
        for (TimeSeries* s :
             {&p.cinstMax, &p.vrxMin, &p.vrxMax, &p.fptUs, &p.latencyUs, &p.rtpOffset, &p.iptMaxNs}) {
            s->clear();
        }
    }
    for (auto& p : audio_) {
        p.total.reset();
        for (TimeSeries* s : {&p.dpvrMaxUs, &p.dpvrMinUs, &p.tsdfUs, &p.iptMaxNs}) {
            s->clear();
        }
    }
    framesTotal_ = FrameCounters{};
    if (videoAnalyzer_) {
        videoAnalyzer_->resetTotals();
    }
    if (audioAnalyzer_) {
        audioAnalyzer_->resetTotals();
    }
    videoSkew_.resetTotals();
    videoPackets_ = videoSwPackets_ = audioPackets_ = audioSwPackets_ = 0;
}

json FlowMonitor::profileToJson(const PacketProfile& profile) const {
    if (profile.empty()) {
        return nullptr;
    }
    const size_t n = profile.index.size();
    const size_t stride = std::max<size_t>(1, (n + kMaxProfilePoints - 1) / kMaxProfilePoints);
    json idx = json::array(), arrival = json::array(), cinst = json::array(), vrx = json::array();
    for (size_t i = 0; i < n; i += stride) {
        int16_t cmax = profile.cinst[i];
        int16_t vmin = profile.vrx[i];
        for (size_t k = i; k < std::min(n, i + stride); ++k) {
            cmax = std::max(cmax, profile.cinst[k]);
            vmin = std::min(vmin, profile.vrx[k]);
        }
        idx.push_back(profile.index[i]);
        arrival.push_back(std::round(profile.arrivalUs[i] * 1000.0f) / 1000.0f);
        cinst.push_back(cmax);
        vrx.push_back(vmin);
    }
    return json{{"rtpTimestamp", profile.rtpTimestamp},
                {"packets", n},
                {"stride", stride},
                {"index", idx},
                {"arrivalUs", arrival},
                {"cinst", cinst},
                {"vrx", vrx},
                {"timing", frameTimingToJson(profile.timing)}};
}

json FlowMonitor::countersJson(const StreamCounters& c) const {
    json paths = json::object();
    for (size_t i = 0; i < kPathCount; ++i) {
        const auto& p = c.paths[i];
        paths[pathKey(static_cast<Path>(i))] =
            json{{"packets", p.packets},     {"bytes", p.bytes},           {"lost", p.lost},
                 {"reordered", p.reordered}, {"duplicates", p.duplicates}, {"errors", p.errors}};
    }
    return json{{"paths", paths},           {"received", c.received},
                {"lost", c.lost},           {"unrecovered", c.unrecovered},
                {"redundant", c.redundant}, {"wrongPayloadType", c.wrongPayloadType},
                {"wrongSsrc", c.wrongSsrc}, {"framesIncomplete", c.framesIncomplete}};
}

json FlowMonitor::countersDelta(const StreamCounters& a, const StreamCounters& b) const {
    StreamCounters d;
    for (size_t i = 0; i < kPathCount; ++i) {
        d.paths[i].packets = a.paths[i].packets - b.paths[i].packets;
        d.paths[i].bytes = a.paths[i].bytes - b.paths[i].bytes;
        d.paths[i].lost = a.paths[i].lost - b.paths[i].lost;
        d.paths[i].reordered = a.paths[i].reordered - b.paths[i].reordered;
        d.paths[i].duplicates = a.paths[i].duplicates - b.paths[i].duplicates;
        d.paths[i].errors = a.paths[i].errors - b.paths[i].errors;
    }
    d.received = a.received - b.received;
    d.lost = a.lost - b.lost;
    d.unrecovered = a.unrecovered - b.unrecovered;
    d.redundant = a.redundant - b.redundant;
    d.wrongPayloadType = a.wrongPayloadType - b.wrongPayloadType;
    d.wrongSsrc = a.wrongSsrc - b.wrongSsrc;
    d.framesIncomplete = a.framesIncomplete - b.framesIncomplete;
    return countersJson(d);
}

json FlowMonitor::videoSnapshot(const SnapshotData& d, double windowSec) const {
    const auto& v = *config_.video;
    json flows{{"P", flowToJson(v.primary)}};
    if (v.redundant) {
        flows["R"] = flowToJson(*v.redundant);
    }
    json params = nullptr;
    std::optional<PassCriteria> pass = d.videoPass;
    if (d.videoParams) {
        const auto& p = *d.videoParams;
        params = json{{"frameTimeNs", p.frameTimeNs},
                      {"trsNs", p.trsNs},
                      {"trOffsetNs", p.trOffsetNs},
                      {"rActive", p.rActive},
                      {"packetsPerFrame", p.packetsPerFrame}};
        if (!pass) {
            pass = p.pass;
        }
    }
    json paths = json::object();
    const size_t count = v.redundant ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        const Path path = static_cast<Path>(i);
        const VideoPath& p = d.video[i];
        json hist = nullptr;
        json profiles = nullptr;
        if (d.hasVideoAnalyzer) {
            const auto& h = d.videoHist[i];
            hist = json{{"iptNs", histogramToJson(h.iptNs)},
                        {"cinst", histogramToJson(h.cinst)},
                        {"vrx", histogramToJson(h.vrx)}};
            profiles =
                json{{"last", profileToJson(d.profileLast[i])}, {"worst", profileToJson(d.profileWorst[i])}};
        }
        const Compliance verdict = p.lastSecondVerdict ? *p.lastSecondVerdict : p.total.worst();
        paths[pathKey(path)] = json{
            {"verdict", p.total.frames() ? json(complianceName(verdict)) : json(nullptr)},
            {"lastFrame", p.lastFrame ? frameTimingToJson(*p.lastFrame) : json(nullptr)},
            {"total", statsToJson(p.total)},
            {"histograms", hist},
            {"profiles", profiles},
            {"series",
             {{"cinstMax", p.cinstMax.toJson(windowSec, kMaxSeriesPoints)},
              {"vrxMin", p.vrxMin.toJson(windowSec, kMaxSeriesPoints)},
              {"vrxMax", p.vrxMax.toJson(windowSec, kMaxSeriesPoints)},
              {"fptUs", p.fptUs.toJson(windowSec, kMaxSeriesPoints)},
              {"latencyUs", p.latencyUs.toJson(windowSec, kMaxSeriesPoints)},
              {"rtpOffset", p.rtpOffset.toJson(windowSec, kMaxSeriesPoints)},
              {"iptMaxNs", p.iptMaxNs.toJson(windowSec, kMaxSeriesPoints)}}},
        };
    }
    json skew = nullptr;
    if (v.redundant) {
        skew = json{{"lastNs", d.skew.hasValue() ? json(d.skew.last()) : json(nullptr)},
                    {"totalNs", rangeToJson(d.skew.total())},
                    {"histogram", histogramToJson(d.skew.histogram())},
                    {"series", d.skewSeries.toJson(windowSec, kMaxSeriesPoints)}};
    }
    return json{{"format", v.format.name()},
                {"payloadType", v.payloadType},
                {"flows", flows},
                {"frameSource", d.mtlVideoFrames ? "mtl" : "analyzer"},
                {"params", params},
                {"pass", pass ? passCriteriaToJson(*pass) : json(nullptr)},
                {"frames", frameCountersToJson(d.frames)},
                {"packets", d.videoPackets},
                {"swTimestampPackets", d.videoSwPackets},
                {"paths", paths},
                {"skew", skew}};
}

json FlowMonitor::audioSnapshot(const SnapshotData& d, double windowSec) const {
    const auto& a = *config_.audio;
    json flows{{"P", flowToJson(a.primary)}};
    if (a.redundant) {
        flows["R"] = flowToJson(*a.redundant);
    }
    json paths = json::object();
    const size_t count = a.redundant ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        const Path path = static_cast<Path>(i);
        const AudioPath& p = d.audio[i];
        json hist = nullptr;
        if (d.hasAudioAnalyzer) {
            const auto& h = d.audioHist[i];
            hist = json{{"dpvrUs", histogramToJson(h.dpvrUs)}, {"iptNs", histogramToJson(h.iptNs)}};
        }
        paths[pathKey(path)] = json{
            {"verdict", p.lastVerdict ? json(complianceName(*p.lastVerdict)) : json(nullptr)},
            {"lastPeriod", p.lastPeriod ? audioTimingToJson(*p.lastPeriod) : json(nullptr)},
            {"total", audioStatsToJson(p.total)},
            {"histograms", hist},
            {"series",
             {{"dpvrMaxUs", p.dpvrMaxUs.toJson(windowSec, kMaxSeriesPoints)},
              {"dpvrMinUs", p.dpvrMinUs.toJson(windowSec, kMaxSeriesPoints)},
              {"tsdfUs", p.tsdfUs.toJson(windowSec, kMaxSeriesPoints)},
              {"iptMaxNs", p.iptMaxNs.toJson(windowSec, kMaxSeriesPoints)}}},
        };
    }
    return json{{"format", a.format.name()},
                {"payloadType", a.payloadType},
                {"flows", flows},
                {"periodSource", d.mtlAudioPeriods ? "mtl" : "analyzer"},
                {"packetTimeUs", packetTimeNs(a.format.ptime) / 1000.0},
                {"samplesPerPacket", a.format.samplesPerPacket()},
                {"pass", d.hasAudioAnalyzer ? audioPassToJson(d.audioPass) : json(nullptr)},
                {"packets", d.audioPackets},
                {"swTimestampPackets", d.audioSwPackets},
                {"paths", paths}};
}

json FlowMonitor::ratesJson(const CounterState& c) const {
    json out = json::object();
    const size_t count = config_.redundant() ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        out[pathKey(static_cast<Path>(i))] =
            c.ratesValid ? json{{"pps", c.pps[i]}, {"bps", c.bps[i]}} : json(nullptr);
    }
    return out;
}

std::optional<Compliance> FlowMonitor::verdictLocked(Path path) const {
    if (config_.video) {
        const VideoPath& p = video_[pathIndex(path)];
        if (p.lastSecondVerdict) {
            return p.lastSecondVerdict;
        }
        return p.total.frames() ? std::optional<Compliance>(p.total.worst()) : std::nullopt;
    }
    return audio_[pathIndex(path)].lastVerdict;
}

json FlowMonitor::summary() {
    std::lock_guard lock(mutex_);
    json flows{{"P", flowToJson(config_.primary())}};
    json verdicts = json::object();
    verdicts["P"] =
        verdictLocked(Path::Primary) ? json(complianceName(*verdictLocked(Path::Primary))) : json(nullptr);
    if (config_.redundant()) {
        flows["R"] = flowToJson(*config_.redundant());
        verdicts["R"] = verdictLocked(Path::Redundant) ? json(complianceName(*verdictLocked(Path::Redundant)))
                                                       : json(nullptr);
    }
    const auto& c = counters_.current;
    return json{
        {"id", config_.id},
        {"label", config_.label},
        {"kind", config_.video ? "video" : "audio"},
        {"format", config_.formatName()},
        {"payloadType", config_.payloadType()},
        {"flows", flows},
        {"verdict", verdicts},
        {"rates", ratesJson(counters_)},
        {"packets", counters_.any ? json(c.received) : json(nullptr)},
        {"lost", counters_.any ? json(std::max(c.lost, c.paths[0].lost + c.paths[1].lost)) : json(nullptr)},
        {"unrecovered", counters_.any ? json(c.unrecovered) : json(nullptr)},
        {"units", config_.video ? framesTotal_.total() : audio_[0].total.periods()}};
}

json FlowMonitor::snapshot(double windowSec) {
    auto d = std::make_unique<SnapshotData>();
    {
        std::lock_guard lock(mutex_);
        d->t = now() - startSec_;
        for (size_t i = 0; i < kPathCount; ++i) {
            const VideoPath& src = video_[i];
            VideoPath& dst = d->video[i];
            dst.total = src.total;
            dst.lastSecondVerdict = src.lastSecondVerdict;
            dst.lastFrame = src.lastFrame;
            dst.cinstMax = src.cinstMax.tail(windowSec);
            dst.vrxMin = src.vrxMin.tail(windowSec);
            dst.vrxMax = src.vrxMax.tail(windowSec);
            dst.fptUs = src.fptUs.tail(windowSec);
            dst.latencyUs = src.latencyUs.tail(windowSec);
            dst.rtpOffset = src.rtpOffset.tail(windowSec);
            dst.iptMaxNs = src.iptMaxNs.tail(windowSec);
            const AudioPath& asrc = audio_[i];
            AudioPath& adst = d->audio[i];
            adst.total = asrc.total;
            adst.lastVerdict = asrc.lastVerdict;
            adst.lastPeriod = asrc.lastPeriod;
            adst.dpvrMaxUs = asrc.dpvrMaxUs.tail(windowSec);
            adst.dpvrMinUs = asrc.dpvrMinUs.tail(windowSec);
            adst.tsdfUs = asrc.tsdfUs.tail(windowSec);
            adst.iptMaxNs = asrc.iptMaxNs.tail(windowSec);
            if (videoAnalyzer_) {
                const Path path = static_cast<Path>(i);
                d->videoHist[i] = videoAnalyzer_->totals(path);
                d->profileLast[i] = videoAnalyzer_->lastProfile(path);
                d->profileWorst[i] = videoAnalyzer_->worstProfile(path);
            }
            if (audioAnalyzer_) {
                d->audioHist[i] = audioAnalyzer_->totals(static_cast<Path>(i));
            }
        }
        d->videoPass = mtlVideoPass_;
        if (videoAnalyzer_) {
            d->hasVideoAnalyzer = true;
            d->videoParams = videoAnalyzer_->params();
        }
        if (audioAnalyzer_) {
            d->hasAudioAnalyzer = true;
            d->audioPass = audioAnalyzer_->pass();
        }
        d->mtlVideoFrames = mtlVideoFrames_;
        d->mtlAudioPeriods = mtlAudioPeriods_;
        d->frames = framesTotal_;
        d->videoPackets = videoPackets_;
        d->videoSwPackets = videoSwPackets_;
        d->audioPackets = audioPackets_;
        d->audioSwPackets = audioSwPackets_;
        d->skew = videoSkew_;
        d->skewSeries = videoSkew_.series().tail(windowSec);
        d->counters = counters_;
    }
    return json{
        {"id", config_.id},
        {"label", config_.label},
        {"kind", config_.video ? "video" : "audio"},
        {"video", config_.video ? videoSnapshot(*d, windowSec) : json(nullptr)},
        {"audio", config_.audio ? audioSnapshot(*d, windowSec) : json(nullptr)},
        {"counters", d->counters.any ? countersJson(d->counters.current) : json(nullptr)},
        {"rates", ratesJson(d->counters)},
    };
}

json FlowMonitor::takeIntervalRecord() {
    std::lock_guard lock(mutex_);
    json paths = json::object();
    const size_t count = config_.redundant() ? 2 : 1;
    json out{{"label", config_.label},
             {"kind", config_.video ? "video" : "audio"},
             {"format", config_.formatName()}};
    for (size_t i = 0; i < count; ++i) {
        const Path path = static_cast<Path>(i);
        json entry;
        if (config_.video) {
            entry = statsToJson(video_[i].interval);
            video_[i].interval.reset();
            if (videoAnalyzer_) {
                auto h = videoAnalyzer_->takeInterval(path);
                entry["histograms"] = json{{"iptNs", histogramSummary(h.iptNs)},
                                           {"cinst", histogramSummary(h.cinst)},
                                           {"vrx", histogramSummary(h.vrx)}};
            }
        } else {
            entry = audioStatsToJson(audio_[i].interval);
            audio_[i].interval.reset();
            if (audioAnalyzer_) {
                auto h = audioAnalyzer_->takeInterval(path);
                entry["histograms"] =
                    json{{"dpvrUs", histogramSummary(h.dpvrUs)}, {"iptNs", histogramSummary(h.iptNs)}};
            }
        }
        paths[pathKey(path)] = entry;
    }
    out["paths"] = paths;
    if (config_.video) {
        out["frames"] = frameCountersToJson(framesInterval_);
        framesInterval_ = FrameCounters{};
        if (config_.video->redundant) {
            out["skewNs"] = rangeToJson(videoSkew_.takeInterval());
        }
    }
    if (counters_.any) {
        out["counters"] = countersDelta(counters_.current, counters_.lastInterval);
        counters_.lastInterval = counters_.current;
    }
    return out;
}

json FlowMonitor::summaryRecord() {
    std::lock_guard lock(mutex_);
    json paths = json::object();
    const size_t count = config_.redundant() ? 2 : 1;
    json out{{"label", config_.label},
             {"kind", config_.video ? "video" : "audio"},
             {"format", config_.formatName()}};
    for (size_t i = 0; i < count; ++i) {
        const Path path = static_cast<Path>(i);
        json entry;
        if (config_.video) {
            entry = statsToJson(video_[i].total);
            if (videoAnalyzer_) {
                const auto& h = videoAnalyzer_->totals(path);
                entry["histograms"] = json{{"iptNs", histogramSummary(h.iptNs)},
                                           {"cinst", histogramSummary(h.cinst)},
                                           {"vrx", histogramSummary(h.vrx)}};
            }
        } else {
            entry = audioStatsToJson(audio_[i].total);
            if (audioAnalyzer_) {
                const auto& h = audioAnalyzer_->totals(path);
                entry["histograms"] =
                    json{{"dpvrUs", histogramSummary(h.dpvrUs)}, {"iptNs", histogramSummary(h.iptNs)}};
            }
        }
        paths[pathKey(path)] = entry;
    }
    out["paths"] = paths;
    if (config_.video) {
        out["frames"] = frameCountersToJson(framesTotal_);
        if (config_.video->redundant) {
            out["skewNs"] = rangeToJson(videoSkew_.total());
        }
    }
    if (counters_.any) {
        out["counters"] = countersJson(counters_.current);
    }
    return out;
}

bool FlowMonitor::matches(const std::string& ip, uint16_t port) const {
    auto hit = [&](const FlowAddress& a) { return a.ip == ip && a.udpPort == port; };
    return hit(config_.primary()) || (config_.redundant() && hit(*config_.redundant()));
}

uint64_t FlowMonitor::received() const {
    std::lock_guard lock(mutex_);
    if (config_.video) {
        return std::max(framesTotal_.total(), video_[0].total.frames());
    }
    return audio_[0].total.periods() + audio_[1].total.periods();
}

} // namespace st2110
