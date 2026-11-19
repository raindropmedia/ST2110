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

namespace {

json causesToJson(const std::map<std::string, uint64_t>& causes) {
    json out = json::object();
    for (const auto& [cause, count] : causes) {
        out[cause] = count;
    }
    return out;
}

} // namespace

json statsToJson(const TimingStats& stats) {
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
        {"failedCauses", causesToJson(stats.failedCauses())},
    };
}

json audioStatsToJson(const AudioStats& stats) {
    return json{
        {"periods", stats.periods()},
        {"packets", stats.packets()},
        {"verdict", complianceName(stats.worst())},
        {"counts",
         {{"narrow", stats.count(Compliance::Narrow)},
          {"wide", stats.count(Compliance::Wide)},
          {"failed", stats.count(Compliance::Failed)}}},
        {"dpvrUs", rangeToJson(stats.dpvrUs())},
        {"iptNs", rangeToJson(stats.iptNs())},
        {"tsdfUs", rangeToJson(stats.tsdfUs())},
        {"failedCauses", causesToJson(stats.failedCauses())},
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

json audioPassToJson(const AudioPassCriteria& c) {
    return json{{"dpvrMaxNarrowUs", c.dpvrMaxNarrowUs},
                {"dpvrMaxWideUs", c.dpvrMaxWideUs},
                {"tsdfMaxNarrowUs", c.tsdfMaxNarrowUs},
                {"tsdfMaxWideUs", c.tsdfMaxWideUs}};
}

json histogramSummary(const Histogram& h) {
    if (!h.configured() || h.count() == 0) {
        return nullptr;
    }
    return json{{"count", h.count()},
                {"min", h.min()},
                {"p50", h.percentile(0.5)},
                {"p99", h.percentile(0.99)},
                {"max", h.max()}};
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
