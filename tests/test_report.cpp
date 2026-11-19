#include "core/Report.h"

#include <doctest/doctest.h>

using namespace st2110;
using nlohmann::json;

namespace {

Options sampleOptions() {
    Options o;
    o.primary.port = "0000:af:00.0";
    o.primary.localIp = "192.168.10.10";
    o.primary.streamIp = "239.10.0.1";
    o.rate = FrameRate::P59_94;
    return o;
}

} // namespace

TEST_CASE("Formatname") {
    auto o = sampleOptions();
    CHECK(formatName(o) == "1920x1080p59.94");
    o.interlaced = true;
    o.rate = FrameRate::P50;
    CHECK(formatName(o) == "1920x1080i50");
}

TEST_CASE("Startzeile enthält weder Port noch lokale Adresse") {
    auto o = sampleOptions();
    json record = startRecord(o, std::nullopt, std::chrono::system_clock::time_point{});
    CHECK(record["type"] == "start");
    CHECK(record["time"] == "1970-01-01T00:00:00.000Z");
    CHECK(record["paths"]["P"]["ip"] == "239.10.0.1");
    CHECK(record["passCriteria"].is_null());
    const std::string text = record.dump();
    CHECK(text.find("0000:af:00.0") == std::string::npos);
    CHECK(text.find("192.168.10.10") == std::string::npos);
}

TEST_CASE("Intervallzeile mit und ohne redundanten Pfad") {
    TimingStats primary;
    FrameTiming f;
    f.compliance = Compliance::Narrow;
    f.cinstMax = 3;
    f.packets = 100;
    primary.add(f);
    FrameCounters frames{1, 0, 0, 0};

    json single = intervalRecord("interval", 1, frames, primary, nullptr, std::chrono::system_clock::now());
    CHECK(single["paths"]["P"]["verdict"] == "narrow");
    CHECK(single["paths"]["P"]["cinst"]["max"] == 3.0);
    CHECK_FALSE(single["paths"].contains("R"));
    CHECK(single["frames"]["total"] == 1);

    TimingStats redundant;
    json both = intervalRecord("summary", 10, frames, primary, &redundant, std::chrono::system_clock::now());
    CHECK(both["type"] == "summary");
    CHECK(both["paths"]["R"]["verdict"] == "failed");
    CHECK(both["paths"]["R"]["cinst"].is_null());
}

TEST_CASE("Grenzwerte") {
    PassCriteria c;
    c.cinstMaxNarrow = 4;
    c.trOffsetNs = 678000;
    json record = passCriteriaRecord(c, std::chrono::system_clock::now());
    CHECK(record["type"] == "passCriteria");
    CHECK(record["passCriteria"]["cinstMaxNarrow"] == 4);
    CHECK(record["passCriteria"]["trOffsetNs"] == 678000);
}

TEST_CASE("Zeitstempel mit Millisekunden") {
    using namespace std::chrono;
    system_clock::time_point t{milliseconds(1'763'550'930'123)};
    CHECK(isoTimestamp(t) == "2025-11-19T11:15:30.123Z");
}
