#include "core/AudioPacketAnalyzer.h"
#include "core/SyntheticSender.h"

#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

namespace {

constexpr uint64_t kBasePacket = 1'760'000'000'000ULL; // ≈ TAI in ms

std::vector<AudioTiming> run(const AudioFormat& format, const SenderFaults& faults, int packets,
                             AudioPacketAnalyzer& analyzer) {
    SyntheticSender sender(VideoFormat{}, format, 3);
    std::vector<AudioTiming> out;
    for (int i = 0; i < packets; ++i) {
        sender.audioPacket(kBasePacket + i, faults, 0.0, [&](uint32_t rtp, uint64_t ns) {
            if (auto done = analyzer.onPacket(Path::Primary, rtp, ns)) {
                out.push_back(*done);
            }
        });
    }
    return out;
}

} // namespace

TEST_CASE("Tonformat") {
    AudioFormat f;
    CHECK(f.samplesPerPacket() == 48);
    CHECK(f.payloadBytes() == 288);
    CHECK(f.name() == "L24/48000/2 1ms");
    f.ptime = PacketTime::P333us;
    CHECK(f.samplesPerPacket() == 16);
    f.ptime = PacketTime::P125us;
    f.sampleRate = 96000;
    CHECK(f.samplesPerPacket() == 12);
    CHECK(parsePacketTime("0.333") == PacketTime::P333us);
    CHECK(parsePacketTime("1.00") == PacketTime::P1ms);
    CHECK_FALSE(parsePacketTime("2"));
    f = AudioFormat{};
    f.channels = 16;
    f.ptime = PacketTime::P4ms;
    CHECK_FALSE(validateAudioFormat(f).empty());
}

TEST_CASE("Grenzwerte wie MTL") {
    auto p = computeAudioPassCriteria(PacketTime::P1ms);
    CHECK(p.dpvrMaxNarrowUs == 3000);
    CHECK(p.dpvrMaxWideUs == 19000);
    CHECK(p.tsdfMaxNarrowUs == 1000);
    CHECK(p.tsdfMaxWideUs == 17000);
}

TEST_CASE("Idealer Tonsender ist narrow, DPVR = halbe Paketzeit") {
    AudioFormat format;
    AudioPacketAnalyzer analyzer(format);
    auto periods = run(format, SenderFaults{}, 1000, analyzer);
    REQUIRE(periods.size() >= 4);
    for (const auto& p : periods) {
        CHECK(p.compliance == Compliance::Narrow);
        CHECK(p.dpvrMinUs == 500);
        CHECK(p.dpvrMaxUs == 500);
        CHECK(p.tsdfUs == 0);
        CHECK(p.packets == 200);
        CHECK(p.iptAvgNs == doctest::Approx(1e6).epsilon(0.001));
    }
    CHECK(analyzer.totals(Path::Primary).dpvrUs.count() == 1000);
}

TEST_CASE("Verzögerung über eine Paketzeit hinaus bleibt richtig (RTP-Überlauf sicher)") {
    AudioFormat format;
    AudioPacketAnalyzer analyzer(format);
    // RTP nahe am 32-Bit-Überlauf
    const uint32_t rtp = 0xFFFFFFF0u;
    const uint64_t ns = 5'000'000'000ULL;
    const double ideal = analyzer.dpvrUs(rtp, ns);
    CHECK(analyzer.dpvrUs(rtp, ns + 2'500'000) == doctest::Approx(ideal + 2500.0));
}

TEST_CASE("Jitter und Verzögerung ergeben wide bzw. failed") {
    AudioFormat format;
    SUBCASE("Jitter 800 µs") {
        AudioPacketAnalyzer analyzer(format);
        SenderFaults faults;
        faults.audioJitterUs = 800;
        auto periods = run(format, faults, 600, analyzer);
        REQUIRE_FALSE(periods.empty());
        CHECK(periods.back().compliance == Compliance::Wide);
        CHECK(periods.back().failedCause == "tsdf exceed max narrow");
    }
    SUBCASE("Verzögerung 25 ms") {
        AudioPacketAnalyzer analyzer(format);
        SenderFaults faults;
        faults.audioDelayUs = 25000;
        auto periods = run(format, faults, 600, analyzer);
        REQUIRE_FALSE(periods.empty());
        CHECK(periods.back().compliance == Compliance::Failed);
        CHECK(periods.back().failedCause == "dpvr exceed max wide");
    }
}

TEST_CASE("Tonstatistik") {
    AudioStats stats;
    AudioTiming a;
    a.compliance = Compliance::Wide;
    a.failedCause = "tsdf exceed max narrow";
    a.tsdfUs = 1200;
    a.packets = 200;
    stats.add(a);
    AudioStats other;
    a.compliance = Compliance::Narrow;
    other.add(a);
    stats.merge(other);
    CHECK(stats.periods() == 2);
    CHECK(stats.worst() == Compliance::Wide);
    CHECK(stats.failedCauses().at("tsdf exceed max narrow") == 1);
}
