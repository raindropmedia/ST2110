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

const char* kSdp =
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
    "a=ptime:0.125\n"
    "m=audio 30000 RTP/AVP 97\n"
    "c=IN IP4 239.100.0.3/64\n"
    "a=rtpmap:97 L16/48000/2\n"
    "a=ptime:1\n";

} // namespace

TEST_CASE("Mindestaufruf mit einem Videoflow liefert Vorgaben") {
    auto result = parseOptions(baseArgs());
    REQUIRE(result.options);
    CHECK(result.errors.empty());
    CHECK(result.warnings.empty());
    const auto& o = *result.options;
    REQUIRE(o.flows.size() == 1);
    REQUIRE(o.flows[0].video);
    CHECK(o.flows[0].primary().ip == "239.10.0.1");
    CHECK(o.flows[0].primary().udpPort == 20000);
    CHECK(o.flows[0].formatName() == "1920x1080p50");
    CHECK(o.flows[0].payloadType() == 96);
    CHECK(o.maxFlows == 8);
    CHECK(o.discoverInterface.empty());
    CHECK(o.httpListen == "127.0.0.1:8090");
}

TEST_CASE("Mehrere Flows mit Schlüssel-Wert-Angaben") {
    auto args = baseArgs();
    args.insert(
        args.end(),
        {"--video",
         "239.10.0.11:20000,red=239.20.0.11:20000,format=1920x1080i50,pt=100,source=10.0.0.5,name=Kamera 2",
         "--audio=239.10.0.2:30000,red=239.20.0.2:30000,format=L16/96000/8,ptime=0.125,name=Pult",
         "--red-port", "0000:af:00.1", "--red-local-ip", "192.168.20.10", "--max-flows", "4"});
    auto result = parseOptions(args);
    REQUIRE(result.options);
    const auto& o = *result.options;
    REQUIRE(o.flows.size() == 3);
    const auto& v = o.flows[1];
    CHECK(v.label == "Kamera 2");
    CHECK(v.video->format.interlaced);
    CHECK(v.payloadType() == 100);
    CHECK(v.primary().sourceIp == "10.0.0.5");
    REQUIRE(v.redundant());
    CHECK(v.redundant()->ip == "239.20.0.11");
    const auto& a = o.flows[2];
    REQUIRE(a.audio);
    CHECK(a.audio->format.bitDepth == 16);
    CHECK(a.audio->format.sampleRate == 96000);
    CHECK(a.audio->format.channels == 8);
    CHECK(a.audio->format.ptime == PacketTime::P125us);
    CHECK(o.maxFlows == 4);
}

TEST_CASE("Flow-Angabe: Fehler werden benannt") {
    FlowConfig f;
    std::string error;
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1", f, error));
    CHECK(error.find("239.1.1.1:20000") != std::string::npos);
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1:20000,format=1080p50", f, error));
    CHECK(error.find("format") != std::string::npos);
    CHECK_FALSE(parseFlowSpec(Media::Audio, "239.1.1.1:20000,ptime=2", f, error));
    CHECK(error.find("0.125") != std::string::npos);
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1:20000,foo=1", f, error));
    CHECK(error.find("Unbekannte") != std::string::npos);
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1:20000,red", f, error));
    CHECK(parseFlowSpec(Media::Audio, "239.1.1.1:5004,format=L24/48000/2", f, error));
    CHECK(f.audio);
    CHECK_FALSE(f.video);
}

TEST_CASE("Alte Einzeloptionen verweisen auf die neue Form") {
    auto args = baseArgs();
    args.insert(args.end(), {"--video-format", "1920x1080p50"});
    CHECK(contains(parseOptions(args).errors, "jetzt als Teil von --video"));
}

TEST_CASE("Hilfe hat Vorrang") {
    auto result = parseOptions({"st2110-monitor", "--help"});
    CHECK(result.helpRequested);
    CHECK(usageText("x").find("--discover") != std::string::npos);
}

TEST_CASE("Fehlende Angaben werden gemeldet") {
    auto result = parseOptions({"st2110-monitor"});
    CHECK_FALSE(result.options);
    CHECK(contains(result.errors, "Kein Flow"));
    CHECK(contains(result.errors, "Port P"));
}

TEST_CASE("Nur Erkennung ohne Flow ist erlaubt") {
    auto result =
        parseOptions({"x", "--port", "0000:af:00.0", "--local-ip", "10.0.0.1", "--discover", "enp1s0f1"});
    REQUIRE(result.options);
    CHECK(result.options->flows.empty());
    CHECK(result.options->discoverInterface == "enp1s0f1");
    CHECK(result.options->sap);
}

TEST_CASE("Ungültige Werte werden abgelehnt") {
    SUBCASE("zu große Tonpakete") {
        auto args = baseArgs();
        args.insert(args.end(), {"--audio", "239.1.1.2:30000,format=L24/48000/16,ptime=4"});
        CHECK(contains(parseOptions(args).errors, "Paket zu groß"));
    }
    SUBCASE("ungerade Breite") {
        auto args = baseArgs();
        args.insert(args.end(), {"--video", "239.1.1.3:20000,format=1921x1080p50"});
        CHECK(contains(parseOptions(args).errors, "Breite"));
    }
    SUBCASE("zu viele Flows") {
        auto args = baseArgs();
        args.insert(args.end(), {"--video", "239.1.1.3:20000", "--max-flows", "1"});
        CHECK(contains(parseOptions(args).errors, "--max-flows"));
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

TEST_CASE("Redundanter Flow verlangt Port R") {
    auto args = baseArgs();
    args.insert(args.end(), {"--video", "239.1.1.3:20000,red=239.20.0.1:20000"});
    CHECK(contains(parseOptions(args).errors, "ohne redundanten Port"));
}

TEST_CASE("kernel:-Port verlangt Verzicht auf Hardware-Zeitstempel und PTP") {
    std::vector<std::string> args{"x", "--port", "kernel:enp1s0f0", "--video", "239.1.1.1:20000"};
    auto strict = parseOptions(args);
    CHECK(contains(strict.errors, "keine Hardware-Zeitstempel"));
    args.insert(args.end(), {"--no-hw-timestamp", "--no-ptp"});
    auto relaxed = parseOptions(args);
    REQUIRE(relaxed.options);
    CHECK(contains(relaxed.warnings, "nicht aussagekräftig"));
}

TEST_CASE("Demo hat mehrere Flows und braucht keinen Port") {
    auto result = parseOptions({"x", "--demo"});
    REQUIRE(result.options);
    const auto& flows = result.options->flows;
    REQUIRE(flows.size() == 4);
    CHECK(flows[0].label == "Kamera 1");
    CHECK(flows[0].redundant());
    CHECK(flows[2].formatName() == "1920x1080i50");
    CHECK(flows[3].formatName() == "L24/48000/8 125us");
    CHECK(result.warnings.empty());
}

TEST_CASE("SDP liefert alle Medien, DUP-Gruppen ergeben P und R") {
    fakeFiles()["kamera.sdp"] = kSdp;
    auto result = parseOptions({"x", "--port", "0000:af:00.0", "--local-ip", "192.168.10.10", "--red-port",
                                "0000:af:00.1", "--red-local-ip", "192.168.20.10", "--sdp", "kamera.sdp"},
                               fakeRead);
    REQUIRE(result.options);
    const auto& flows = result.options->flows;
    REQUIRE(flows.size() == 3);
    CHECK(flows[0].label == "Kamera 1 Video");
    CHECK(flows[0].primary().sourceIp == "10.0.0.5");
    REQUIRE(flows[0].redundant());
    CHECK(flows[0].redundant()->ip == "239.101.0.1");
    CHECK(flows[0].formatName() == "1920x1080i59.94");
    CHECK(flows[1].label == "Kamera 1 Ton");
    CHECK(flows[1].audio->format.channels == 8);
    CHECK(flows[2].label == "Kamera 1 Ton 2");
    CHECK(flows[2].audio->format.bitDepth == 16);
}

TEST_CASE("Fehlende SDP-Datei") {
    CHECK(contains(parseOptions({"x", "--sdp", "fehlt.sdp"}, fakeRead).errors, "nicht gelesen"));
}

TEST_CASE("Adressprüfungen") {
    CHECK(isPciAddress("0000:af:00.0"));
    CHECK_FALSE(isPciAddress("0000:af:00.8"));
    CHECK(isKernelPort("kernel:eth0"));
    CHECK(isMulticastIpv4("239.255.0.1"));
    CHECK_FALSE(isMulticastIpv4("192.168.0.1"));
    auto flow = parseFlowAddress("239.1.2.3:5004");
    REQUIRE(flow);
    CHECK(flow->udpPort == 5004);
    CHECK_FALSE(parseFlowAddress("239.1.2.3:0"));
    std::string host;
    uint16_t port = 0;
    CHECK(parseListen("0.0.0.0:80", host, port));
}
