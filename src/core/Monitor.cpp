#include "core/Monitor.h"

#include "core/Report.h"

#include <algorithm>
#include <cmath>

namespace st2110 {

using nlohmann::json;

namespace {

constexpr size_t kMaxEvents = 300;
constexpr size_t kSeriesCapacity = 12000; // 50 Hz x 240 s
constexpr size_t kMaxSeriesPoints = 1200;
constexpr size_t kMaxProfilePoints = 2000;

json flowToJson(const FlowAddress& f) {
    json out{{"ip", f.ip}, {"udpPort", f.udpPort}};
    if (!f.sourceIp.empty()) {
        out["sourceIp"] = f.sourceIp;
    }
    return out;
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

Monitor::Monitor(MonitorConfig config, Clock clock)
    : config_(std::move(config)), clock_(std::move(clock)), startWall_(std::chrono::system_clock::now()) {
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
    }
    for (auto& p : video_) {
        for (TimeSeries* s :
             {&p.cinstMax, &p.vrxMin, &p.vrxMax, &p.fptUs, &p.latencyUs, &p.rtpOffset, &p.iptMaxNs}) {
            *s = TimeSeries(kSeriesCapacity);
        }
    }
}

void Monitor::addEvent(const std::string& level, const std::string& text) {
    std::lock_guard lock(mutex_);
    addEventLocked(level, text);
}

void Monitor::addEventLocked(const std::string& level, const std::string& text) {
    const double t = now();
    const auto wall = startWall_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                       std::chrono::duration<double>(t - startSec_));
    events_.push_back(Event{t - startSec_, isoTimestamp(wall), level, text});
    while (events_.size() > kMaxEvents) {
        events_.pop_front();
    }
}

void Monitor::onVideoPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, int packetIndex, bool hw) {
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

void Monitor::onVideoFrame(const VideoFrameReport& report) {
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

void Monitor::setVideoPassCriteria(const PassCriteria& criteria) {
    std::lock_guard lock(mutex_);
    mtlVideoPass_ = criteria;
}

void Monitor::setVideoPacketsPerFrame(int packets) {
    std::lock_guard lock(mutex_);
    if (videoAnalyzer_) {
        videoAnalyzer_->setPacketsPerFrame(packets);
    }
}

void Monitor::recordVideoFrame(Path path, const FrameTiming& f) {
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

void Monitor::onAudioPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, bool hw) {
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

void Monitor::onAudioPeriod(Path path, const AudioTiming& timing) {
    std::lock_guard lock(mutex_);
    mtlAudioPeriods_ = true;
    recordAudioPeriod(path, timing);
}

void Monitor::recordAudioPeriod(Path path, const AudioTiming& a) {
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

void Monitor::onPtpSync(int64_t deltaNs, int16_t utcOffset) {
    std::lock_guard lock(mutex_);
    const double t = now() - startSec_;
    ptp_.onSync(t, deltaNs, utcOffset);
    checkPtpLocked(t);
}

void Monitor::checkPtpLocked(double t) {
    if (!config_.ptp && !config_.demo) {
        return;
    }
    const PtpState state = ptp_.state(t);
    if (ptpStateKnown_ && state == lastPtpState_) {
        return;
    }
    if (!ptpStateKnown_ && state != PtpState::Locked) {
        // Anlaufphase: "synchronisiert" nicht melden, "kein Sync" erst nach einigen Sekunden.
        if (state == PtpState::NoSync && t >= 5.0) {
            addEventLocked("error", "Kein PTP-Sync vom Grandmaster");
            ptpStateKnown_ = true;
            lastPtpState_ = state;
        }
        return;
    }
    switch (state) {
    case PtpState::Locked:
        addEventLocked("info", "PTP eingerastet (Korrektur < " +
                                   std::to_string(static_cast<int>(PtpMonitor::Settings{}.lockThresholdNs)) +
                                   " ns)");
        break;
    case PtpState::Syncing:
        addEventLocked("warn",
                       "PTP nicht eingerastet (Korrektur " + std::to_string(ptp_.lastDeltaNs()) + " ns)");
        break;
    case PtpState::NoSync:
        addEventLocked("error", "Kein PTP-Sync vom Grandmaster");
        break;
    }
    lastPtpState_ = state;
    ptpStateKnown_ = true;
}

void Monitor::setCounters(Media media, const StreamCounters& c) {
    std::lock_guard lock(mutex_);
    CounterState& s = counters_[media == Media::Video ? 0 : 1];
    const char* name = media == Media::Video ? "Video" : "Ton";
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
    } else {
        s.lastInterval = c;
    }
    s.current = c;
    s.any = true;
}

void Monitor::reset() {
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
    ptp_.resetTotals();
    videoPackets_ = videoSwPackets_ = audioPackets_ = audioSwPackets_ = 0;
    events_.clear();
    addEventLocked("info", "Messwerte zurückgesetzt");
}

json Monitor::profileToJson(const PacketProfile& profile) const {
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

json Monitor::countersJson(const StreamCounters& c) const {
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

json Monitor::countersDelta(const StreamCounters& a, const StreamCounters& b) const {
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

json Monitor::videoSnapshot(const SnapshotData& d, double windowSec) const {
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

json Monitor::audioSnapshot(const SnapshotData& d, double windowSec) const {
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

json Monitor::snapshot(double windowSec) {
    auto d = std::make_unique<SnapshotData>();
    {
        std::lock_guard lock(mutex_);
        d->t = now() - startSec_;
        checkPtpLocked(d->t);
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
        d->ptp = ptp_;
        d->counters = counters_;
        const size_t n = std::min<size_t>(events_.size(), 100);
        d->events.assign(events_.end() - static_cast<std::ptrdiff_t>(n), events_.end());
    }

    json events = json::array();
    for (auto it = d->events.rbegin(); it != d->events.rend(); ++it) {
        events.push_back(json{{"t", it->t}, {"time", it->time}, {"level", it->level}, {"text", it->text}});
    }
    return json{
        {"time", isoTimestamp(std::chrono::system_clock::now())},
        {"uptimeSec", d->t},
        {"demo", config_.demo},
        {"hwTimestamp", config_.hwTimestamp},
        {"ptpEnabled", config_.ptp},
        {"video", config_.video ? videoSnapshot(*d, windowSec) : json(nullptr)},
        {"audio", config_.audio ? audioSnapshot(*d, windowSec) : json(nullptr)},
        {"ptp", d->ptp.toJson(d->t, windowSec, kMaxSeriesPoints)},
        {"counters",
         {{"video", d->counters[0].any ? countersJson(d->counters[0].current) : json(nullptr)},
          {"audio", d->counters[1].any ? countersJson(d->counters[1].current) : json(nullptr)}}},
        {"events", events},
    };
}

json Monitor::startRecord() {
    std::lock_guard lock(mutex_);
    json video = nullptr;
    if (config_.video) {
        const auto& v = *config_.video;
        json flows{{"P", flowToJson(v.primary)}};
        if (v.redundant) {
            flows["R"] = flowToJson(*v.redundant);
        }
        video = json{{"format", v.format.name()}, {"payloadType", v.payloadType}, {"flows", flows}};
    }
    json audio = nullptr;
    if (config_.audio) {
        const auto& a = *config_.audio;
        json flows{{"P", flowToJson(a.primary)}};
        if (a.redundant) {
            flows["R"] = flowToJson(*a.redundant);
        }
        audio = json{{"format", a.format.name()}, {"payloadType", a.payloadType}, {"flows", flows}};
    }
    return json{{"type", "start"},      {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"demo", config_.demo}, {"hwTimestamp", config_.hwTimestamp},
                {"ptp", config_.ptp},   {"video", video},
                {"audio", audio}};
}

json Monitor::takeIntervalRecord() {
    std::lock_guard lock(mutex_);
    const double t = now() - startSec_;
    checkPtpLocked(t);
    json video = nullptr;
    if (config_.video) {
        json paths = json::object();
        const size_t count = config_.video->redundant ? 2 : 1;
        for (size_t i = 0; i < count; ++i) {
            const Path path = static_cast<Path>(i);
            json entry = statsToJson(video_[i].interval);
            if (videoAnalyzer_) {
                auto h = videoAnalyzer_->takeInterval(path);
                entry["histograms"] = json{{"iptNs", histogramSummary(h.iptNs)},
                                           {"cinst", histogramSummary(h.cinst)},
                                           {"vrx", histogramSummary(h.vrx)}};
            }
            paths[pathKey(path)] = entry;
            video_[i].interval.reset();
        }
        video = json{{"frames", frameCountersToJson(framesInterval_)}, {"paths", paths}};
        if (config_.video->redundant) {
            video["skewNs"] = rangeToJson(videoSkew_.takeInterval());
        }
        framesInterval_ = FrameCounters{};
    }
    json audio = nullptr;
    if (config_.audio) {
        json paths = json::object();
        const size_t count = config_.audio->redundant ? 2 : 1;
        for (size_t i = 0; i < count; ++i) {
            const Path path = static_cast<Path>(i);
            json entry = audioStatsToJson(audio_[i].interval);
            if (audioAnalyzer_) {
                auto h = audioAnalyzer_->takeInterval(path);
                entry["histograms"] =
                    json{{"dpvrUs", histogramSummary(h.dpvrUs)}, {"iptNs", histogramSummary(h.iptNs)}};
            }
            paths[pathKey(path)] = entry;
            audio_[i].interval.reset();
        }
        audio = json{{"paths", paths}};
    }
    json counters = json::object();
    for (size_t i = 0; i < 2; ++i) {
        if (counters_[i].any) {
            counters[i == 0 ? "video" : "audio"] =
                countersDelta(counters_[i].current, counters_[i].lastInterval);
            counters_[i].lastInterval = counters_[i].current;
        }
    }
    return json{{"type", "interval"},
                {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"uptimeSec", t},
                {"video", video},
                {"audio", audio},
                {"ptp",
                 {{"state", ptpStateName(ptp_.state(t))},
                  {"syncs", ptp_.syncCount()},
                  {"deltaNs", rangeToJson(ptp_.takeInterval())}}},
                {"counters", counters}};
}

json Monitor::summaryRecord() {
    std::lock_guard lock(mutex_);
    const double t = now() - startSec_;
    json video = nullptr;
    if (config_.video) {
        json paths = json::object();
        const size_t count = config_.video->redundant ? 2 : 1;
        for (size_t i = 0; i < count; ++i) {
            const Path path = static_cast<Path>(i);
            json entry = statsToJson(video_[i].total);
            if (videoAnalyzer_) {
                const auto& h = videoAnalyzer_->totals(path);
                entry["histograms"] = json{{"iptNs", histogramSummary(h.iptNs)},
                                           {"cinst", histogramSummary(h.cinst)},
                                           {"vrx", histogramSummary(h.vrx)}};
            }
            paths[pathKey(path)] = entry;
        }
        video = json{{"frames", frameCountersToJson(framesTotal_)}, {"paths", paths}};
        if (config_.video->redundant) {
            video["skewNs"] = rangeToJson(videoSkew_.total());
        }
    }
    json audio = nullptr;
    if (config_.audio) {
        json paths = json::object();
        const size_t count = config_.audio->redundant ? 2 : 1;
        for (size_t i = 0; i < count; ++i) {
            json entry = audioStatsToJson(audio_[i].total);
            if (audioAnalyzer_) {
                const auto& h = audioAnalyzer_->totals(static_cast<Path>(i));
                entry["histograms"] =
                    json{{"dpvrUs", histogramSummary(h.dpvrUs)}, {"iptNs", histogramSummary(h.iptNs)}};
            }
            paths[pathKey(static_cast<Path>(i))] = entry;
        }
        audio = json{{"paths", paths}};
    }
    json counters = json::object();
    for (size_t i = 0; i < 2; ++i) {
        if (counters_[i].any) {
            counters[i == 0 ? "video" : "audio"] = countersJson(counters_[i].current);
        }
    }
    return json{{"type", "summary"},
                {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"durationSec", t},
                {"video", video},
                {"audio", audio},
                {"ptp",
                 {{"state", ptpStateName(ptp_.state(t))},
                  {"syncs", ptp_.syncCount()},
                  {"deltaNs", rangeToJson(ptp_.totalDelta())}}},
                {"counters", counters}};
}

uint64_t Monitor::videoFrames() const {
    std::lock_guard lock(mutex_);
    uint64_t frames = framesTotal_.total();
    return std::max(frames, video_[0].total.frames());
}

uint64_t Monitor::audioPeriods() const {
    std::lock_guard lock(mutex_);
    return audio_[0].total.periods() + audio_[1].total.periods();
}

} // namespace st2110
