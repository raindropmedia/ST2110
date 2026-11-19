#include "core/Report.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace st2110 {

using nlohmann::json;

json rangeToJson(const Range& range) {
    if (range.empty()) {
        return nullptr;
    }
    return json{{"min", range.min()}, {"max", range.max()}, {"mean", range.mean()}};
}

json statsToJson(const TimingStats& stats) {
    json causes = json::object();
    for (const auto& [cause, count] : stats.failedCauses()) {
        causes[cause] = count;
    }
    return json{
        {"frames", stats.frames()},
        {"packets", stats.packets()},
        {"verdict", complianceName(stats.worst())},
        {"counts",
         {{"narrow", stats.count(Compliance::Narrow)},
          {"wide", stats.count(Compliance::Wide)},
          {"failed", stats.count(Compliance::Failed)}}},
        {"cinst", rangeToJson(stats.cinst())},
        {"vrx", rangeToJson(stats.vrx())},
        {"iptNs", rangeToJson(stats.iptNs())},
        {"fptNs", rangeToJson(stats.fptNs())},
        {"latencyNs", rangeToJson(stats.latencyNs())},
        {"rtpOffset", rangeToJson(stats.rtpOffset())},
        {"rtpTsDelta", rangeToJson(stats.rtpTsDelta())},
        {"failedCauses", causes},
    };
}

json passCriteriaToJson(const PassCriteria& c) {
    return json{
        {"cinstMaxNarrow", c.cinstMaxNarrow}, {"cinstMaxWide", c.cinstMaxWide},
        {"vrxMaxNarrow", c.vrxMaxNarrow},     {"vrxMaxWide", c.vrxMaxWide},
        {"trOffsetNs", c.trOffsetNs},         {"latencyMinNs", c.latencyMinNs},
        {"latencyMaxNs", c.latencyMaxNs},     {"rtpOffsetMin", c.rtpOffsetMin},
        {"rtpOffsetMax", c.rtpOffsetMax},     {"rtpTsDeltaMin", c.rtpTsDeltaMin},
        {"rtpTsDeltaMax", c.rtpTsDeltaMax},
    };
}

std::string formatName(const Options& options) {
    std::ostringstream out;
    out << options.width << 'x' << options.height << (options.interlaced ? 'i' : 'p')
        << frameRateName(options.rate);
    return out.str();
}

namespace {

json pathToJson(const PathOptions& path) {
    json out{{"ip", path.streamIp}, {"udpPort", path.udpPort}};
    if (!path.sourceIp.empty()) {
        out["sourceIp"] = path.sourceIp;
    }
    return out;
}

} // namespace

json startRecord(const Options& options, const std::optional<PassCriteria>& criteria,
                 std::chrono::system_clock::time_point now) {
    json paths{{"P", pathToJson(options.primary)}};
    if (options.redundant) {
        paths["R"] = pathToJson(*options.redundant);
    }
    return json{
        {"type", "start"},
        {"time", isoTimestamp(now)},
        {"format", formatName(options)},
        {"payloadType", options.payloadType},
        {"hwTimestamp", options.hwTimestamp},
        {"ptp", options.ptp},
        {"paths", paths},
        {"passCriteria", criteria ? passCriteriaToJson(*criteria) : json(nullptr)},
    };
}

json passCriteriaRecord(const PassCriteria& criteria, std::chrono::system_clock::time_point now) {
    return json{{"type", "passCriteria"},
                {"time", isoTimestamp(now)},
                {"passCriteria", passCriteriaToJson(criteria)}};
}

json intervalRecord(const std::string& type, unsigned intervalSec, const FrameCounters& frames,
                    const TimingStats& primary, const TimingStats* redundant,
                    std::chrono::system_clock::time_point now) {
    json paths{{"P", statsToJson(primary)}};
    if (redundant != nullptr) {
        paths["R"] = statsToJson(*redundant);
    }
    return json{
        {"type", type},
        {"time", isoTimestamp(now)},
        {"intervalSec", intervalSec},
        {"frames",
         {{"total", frames.total()},
          {"complete", frames.complete},
          {"reconstructed", frames.reconstructed},
          {"corrupted", frames.corrupted},
          {"dropped", frames.dropped}}},
        {"paths", paths},
    };
}

std::string isoTimestamp(std::chrono::system_clock::time_point time) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << (millis < 0 ? millis + 1000 : millis) << 'Z';
    return out.str();
}

} // namespace st2110
