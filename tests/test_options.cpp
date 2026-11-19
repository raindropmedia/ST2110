#include "core/Options.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <map>

using namespace st2110;

namespace {

std::vector<std::string> baseArgs() {
    return {"st2110-monitor", "--port",  "0000:af:00.0",    "--local-ip",
            "192.168.10.10",  "--video", "239.10.0.1:20000"};
}

bool contains(const std::vector<std::string>& messages, const std::string& part) {
    return std::any_of(messages.begin(), messages.end(),
                       [&](const std::string& m) { return m.find(part) != std::string::npos; });
}

std::map<std::string, std::string>& fakeFiles() {
    static std::map<std::string, std::string> files;
    return files;
}

bool fakeRead(const std::string& path, std::string& content) {
    auto it = fakeFiles().find(path);
    if (it == fakeFiles().end()) {
        return false;
    }
    content = it->second;
    return true;
}

} // namespace

TEST_CASE("Mindestaufruf mit Video liefert Vorgaben") {
    auto result = parseOptions(baseArgs());
    REQUIRE(result.options);
    CHECK(result.errors.empty());
    CHECK(result.warnings.empty());
    const auto& o = *result.options;
    REQUIRE(o.video);
    CHECK(o.video->primary.ip == "239.10.0.1");
    CHECK(o.video->primary.udpPort == 20000);
    CHECK(o.video->format.name() == "1920x1080p50");
    CHECK(o.video->payloadType == 96);
    CHECK_FALSE(o.audio);
    CHECK(o.hwTimestamp);
    CHECK(o.ptp);
    CHECK(o.httpListen == "127.0.0.1:8090");
}

TEST_CASE("Video und Ton mit Format, Paketzeit und Redundanz") {
    auto args = baseArgs();
    args.insert(args.end(),
                {"--video-format=1920x1080i50", "--video-pt", "100", "--video-red", "239.20.0.1:20000",
                 "--audio", "239.10.0.2:30000", "--audio-red=239.20.0.2:30000", "--audio-format",
                 "L16/96000/8", "--audio-ptime", "0.125", "--red-port", "0000:af:00.1", "--red-local-ip",
                 "192.168.20.10", "--video-source", "10.0.0.5"});
    auto result = parseOptions(args);
    REQUIRE(result.options);
    const auto& o = *result.options;
    CHECK(o.video->format.interlaced);
    CHECK(o.video->format.rate == FrameRate::P50);
    CHECK(o.video->payloadType == 100);
    CHECK(o.video->primary.sourceIp == "10.0.0.5");
    REQUIRE(o.video->redundant);
    CHECK(o.video->redundant->ip == "239.20.0.1");
    REQUIRE(o.audio);
    CHECK(o.audio->format.bitDepth == 16);
    CHECK(o.audio->format.sampleRate == 96000);
    CHECK(o.audio->format.channels == 8);
    CHECK(o.audio->format.ptime == PacketTime::P125us);
    CHECK(o.audio->redundant);
    REQUIRE(o.redundantPort);
    CHECK(o.redundantPort->port == "0000:af:00.1");
}

TEST_CASE("Hilfe hat Vorrang") {
    auto result = parseOptions({"st2110-monitor", "--help"});
    CHECK(result.helpRequested);
    CHECK(usageText("x").find("--sdp") != std::string::npos);
}

TEST_CASE("Fehlende Angaben werden gemeldet") {
    auto result = parseOptions({"st2110-monitor"});
    CHECK_FALSE(result.options);
    CHECK(contains(result.errors, "Kein Strom"));
    CHECK(contains(result.errors, "Port P"));
}

TEST_CASE("Ungültige Werte werden abgelehnt") {
    SUBCASE("unbekannte Option") {
        auto args = baseArgs();
        args.push_back("--foo");
        CHECK(contains(parseOptions(args).errors, "Unbekannte Option"));
    }
    SUBCASE("Videoformat") {
        auto args = baseArgs();
        args.insert(args.end(), {"--video-format", "1080p50"});
        CHECK(contains(parseOptions(args).errors, "BREITExHÖHE"));
    }
    SUBCASE("Adresse ohne Port") {
        CHECK(contains(parseOptions({"x", "--video", "239.1.1.1"}).errors, "239.1.1.1:20000"));
    }
    SUBCASE("Paketzeit") {
        auto args = baseArgs();
        args.insert(args.end(), {"--audio", "239.1.1.2:30000", "--audio-ptime", "2"});
        CHECK(contains(parseOptions(args).errors, "0.125"));
    }
    SUBCASE("zu große Tonpakete") {
        auto args = baseArgs();
        args.insert(args.end(),
                    {"--audio", "239.1.1.2:30000", "--audio-format", "L24/48000/16", "--audio-ptime", "4"});
        CHECK(contains(parseOptions(args).errors, "Paket zu groß"));
    }
    SUBCASE("ungerade Breite") {
        auto args = baseArgs();
        args.insert(args.end(), {"--video-format", "1921x1080p50"});
        CHECK(contains(parseOptions(args).errors, "Breite"));
    }
    SUBCASE("PCI-Adresse") {
        CHECK(contains(
            parseOptions({"x", "--port", "af:00.0", "--local-ip", "10.0.0.1", "--video", "239.1.1.1:2"})
                .errors,
            "weder eine PCI-Adresse"));
    }
    SUBCASE("Schalter mit Wert") {
        auto args = baseArgs();
        args.push_back("--demo=1");
        CHECK(contains(parseOptions(args).errors, "erwartet keinen Wert"));
    }
    SUBCASE("HTTP-Adresse") {
        auto args = baseArgs();
        args.insert(args.end(), {"--http", "localhost"});
        CHECK(contains(parseOptions(args).errors, "--http"));
    }
}

TEST_CASE("Redundanter Strom verlangt Port R") {
    auto args = baseArgs();
    args.insert(args.end(), {"--video-red", "239.20.0.1:20000"});
    CHECK(contains(parseOptions(args).errors, "ohne redundanten Port"));
}

TEST_CASE("kernel:-Port verlangt Verzicht auf Hardware-Zeitstempel und PTP") {
    std::vector<std::string> args{"x", "--port", "kernel:enp1s0f0", "--video", "239.1.1.1:20000"};
    auto strict = parseOptions(args);
    CHECK(contains(strict.errors, "keine Hardware-Zeitstempel"));
    CHECK(contains(strict.errors, "--no-ptp"));
    args.insert(args.end(), {"--no-hw-timestamp", "--no-ptp"});
    auto relaxed = parseOptions(args);
    REQUIRE(relaxed.options);
    CHECK(contains(relaxed.warnings, "nicht aussagekräftig"));
}

TEST_CASE("Demo braucht weder Port noch Strom") {
    auto result = parseOptions({"x", "--demo"});
    REQUIRE(result.options);
    CHECK(result.options->demo);
    REQUIRE(result.options->video);
    REQUIRE(result.options->video->redundant);
    REQUIRE(result.options->audio);
    CHECK(result.warnings.empty());
}

TEST_CASE("Web-Oberfläche nach außen ergibt einen Hinweis, --no-http schaltet ab") {
    auto args = baseArgs();
    args.insert(args.end(), {"--http", "0.0.0.0:8090"});
    CHECK(contains(parseOptions(args).warnings, "ohne Anmeldung"));
    auto off = baseArgs();
    off.push_back("--no-http");
    CHECK(parseOptions(off).options->httpListen.empty());
}

TEST_CASE("SDP liefert Video und Ton mit DUP-Gruppen") {
    fakeFiles()["kamera.sdp"] =
        "v=0\n"
        "o=- 1 1 IN IP4 10.0.0.5\n"
        "s=Kamera 1\n"
        "t=0 0\n"
        "a=group:DUP primary secondary\n"
        "m=video 20000 RTP/AVP 96\n"
        "c=IN IP4 239.100.0.1/64\n"
        "a=source-filter: incl IN IP4 239.100.0.1 10.0.0.5\n"
        "a=rtpmap:96 raw/90000\n"
        "a=fmtp:96 sampling=YCbCr-4:2:2; width=1920; height=1080; exactframerate=30000/1001; "
        "depth=10; TCS=SDR; colorimetry=BT709; PM=2110GPM; SSN=ST2110-20:2017; TP=2110TPN; interlace\n"
        "a=mid:primary\n"
        "m=video 20000 RTP/AVP 96\n"
        "c=IN IP4 239.101.0.1/64\n"
        "a=rtpmap:96 raw/90000\n"
        "a=fmtp:96 sampling=YCbCr-4:2:2; width=1920; height=1080; exactframerate=30000/1001; "
        "depth=10; interlace\n"
        "a=mid:secondary\n"
        "m=audio 30000 RTP/AVP 97\n"
        "c=IN IP4 239.100.0.2/64\n"
        "a=rtpmap:97 L24/48000/8\n"
        "a=ptime:0.125\n";
    auto result = parseOptions({"x", "--port", "0000:af:00.0", "--local-ip", "192.168.10.10", "--red-port",
                                "0000:af:00.1", "--red-local-ip", "192.168.20.10", "--sdp", "kamera.sdp"},
                               fakeRead);
    REQUIRE(result.options);
    const auto& o = *result.options;
    REQUIRE(o.video);
    CHECK(o.video->primary.ip == "239.100.0.1");
    CHECK(o.video->primary.sourceIp == "10.0.0.5");
    REQUIRE(o.video->redundant);
    CHECK(o.video->redundant->ip == "239.101.0.1");
    CHECK(o.video->format.name() == "1920x1080i59.94");
    REQUIRE(o.audio);
    CHECK(o.audio->format.channels == 8);
    CHECK(o.audio->format.ptime == PacketTime::P125us);
    CHECK_FALSE(o.audio->redundant);
}

TEST_CASE("Fehlende SDP-Datei") {
    CHECK(contains(parseOptions({"x", "--sdp", "fehlt.sdp"}, fakeRead).errors, "nicht gelesen"));
}

TEST_CASE("Adressprüfungen") {
    CHECK(isPciAddress("0000:af:00.0"));
    CHECK_FALSE(isPciAddress("0000:af:00.8"));
    CHECK(isKernelPort("kernel:eth0"));
    CHECK_FALSE(isKernelPort("kernel:"));
    CHECK(isMulticastIpv4("239.255.0.1"));
    CHECK_FALSE(isMulticastIpv4("192.168.0.1"));
    auto flow = parseFlowAddress("239.1.2.3:5004");
    REQUIRE(flow);
    CHECK(flow->udpPort == 5004);
    CHECK_FALSE(parseFlowAddress("239.1.2.3:0"));
    std::string host;
    uint16_t port = 0;
    CHECK(parseListen("0.0.0.0:80", host, port));
    CHECK(port == 80);
}
