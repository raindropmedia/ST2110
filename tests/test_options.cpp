#include "core/Options.h"

#include <algorithm>
#include <doctest/doctest.h>

using namespace st2110;

namespace {

std::vector<std::string> baseArgs() {
    return {"st20-timing", "--port", "0000:af:00.0", "--local-ip", "192.168.10.10", "--ip", "239.10.0.1"};
}

bool contains(const std::vector<std::string>& messages, const std::string& part) {
    return std::any_of(messages.begin(), messages.end(),
                       [&](const std::string& m) { return m.find(part) != std::string::npos; });
}

} // namespace

TEST_CASE("Mindestaufruf mit DPDK-Port liefert Vorgaben") {
    auto result = parseOptions(baseArgs());
    REQUIRE(result.options);
    CHECK(result.errors.empty());
    CHECK(result.warnings.empty());
    const auto& o = *result.options;
    CHECK(o.primary.port == "0000:af:00.0");
    CHECK(o.primary.udpPort == 20000);
    CHECK(o.width == 1920);
    CHECK(o.height == 1080);
    CHECK(o.rate == FrameRate::P50);
    CHECK(o.hwTimestamp);
    CHECK(o.ptp);
    CHECK_FALSE(o.redundant.has_value());
}

TEST_CASE("Werte mit = und Leerzeichen") {
    auto args = baseArgs();
    args.insert(args.end(), {"--fps=59.94", "--width", "1280", "--height=720", "--udp-port", "20002",
                             "--payload-type=96", "--interlaced", "--duration", "10"});
    auto result = parseOptions(args);
    REQUIRE(result.options);
    CHECK(result.options->rate == FrameRate::P59_94);
    CHECK(result.options->width == 1280);
    CHECK(result.options->height == 720);
    CHECK(result.options->primary.udpPort == 20002);
    CHECK(result.options->payloadType == 96);
    CHECK(result.options->interlaced);
    CHECK(result.options->durationSec == 10);
}

TEST_CASE("Hilfe hat Vorrang") {
    auto result = parseOptions({"st20-timing", "--help"});
    CHECK(result.helpRequested);
    CHECK_FALSE(result.options);
    CHECK(usageText("st20-timing").find("--port") != std::string::npos);
}

TEST_CASE("Fehlende Pflichtangaben werden gemeldet") {
    auto result = parseOptions({"st20-timing"});
    CHECK_FALSE(result.options);
    CHECK(contains(result.errors, "Port fehlt"));
    CHECK(contains(result.errors, "Adresse des Stroms"));
}

TEST_CASE("Ungültige Werte werden abgelehnt") {
    SUBCASE("unbekannte Option") {
        auto args = baseArgs();
        args.push_back("--foo");
        CHECK(contains(parseOptions(args).errors, "Unbekannte Option"));
    }
    SUBCASE("Bildrate") {
        auto args = baseArgs();
        args.insert(args.end(), {"--fps", "48"});
        CHECK(contains(parseOptions(args).errors, "erlaubt:"));
    }
    SUBCASE("UDP-Port außerhalb 16 Bit") {
        auto args = baseArgs();
        args.insert(args.end(), {"--udp-port", "70000"});
        CHECK(contains(parseOptions(args).errors, "--udp-port"));
    }
    SUBCASE("Payload-Typ über 127") {
        auto args = baseArgs();
        args.insert(args.end(), {"--payload-type", "200"});
        CHECK(contains(parseOptions(args).errors, "Payload-Typ"));
    }
    SUBCASE("ungerade Breite") {
        auto args = baseArgs();
        args.insert(args.end(), {"--width", "1921"});
        CHECK(contains(parseOptions(args).errors, "Breite"));
    }
    SUBCASE("PCI-Adresse") {
        auto result =
            parseOptions({"st20-timing", "--port", "af:00.0", "--local-ip", "10.0.0.1", "--ip", "239.1.1.1"});
        CHECK(contains(result.errors, "weder eine PCI-Adresse"));
    }
    SUBCASE("Wert fehlt am Ende") {
        auto args = baseArgs();
        args.push_back("--udp-port");
        CHECK(contains(parseOptions(args).errors, "erwartet einen Wert"));
    }
    SUBCASE("Schalter mit Wert") {
        auto args = baseArgs();
        args.push_back("--interlaced=1");
        CHECK(contains(parseOptions(args).errors, "erwartet keinen Wert"));
    }
}

TEST_CASE("kernel:-Port verlangt Verzicht auf Hardware-Zeitstempel und PTP") {
    std::vector<std::string> args{"st20-timing", "--port", "kernel:enp1s0f0", "--ip", "239.1.1.1"};
    auto strict = parseOptions(args);
    CHECK_FALSE(strict.options);
    CHECK(contains(strict.errors, "keine Hardware-Zeitstempel"));
    CHECK(contains(strict.errors, "--no-ptp"));

    args.insert(args.end(), {"--no-hw-timestamp", "--no-ptp"});
    auto relaxed = parseOptions(args);
    REQUIRE(relaxed.options);
    CHECK(contains(relaxed.warnings, "nicht aussagekräftig"));
}

TEST_CASE("Redundanter Pfad übernimmt den UDP-Port des Primärpfads") {
    auto args = baseArgs();
    args.insert(args.end(), {"--udp-port", "20010", "--red-port", "0000:af:00.1", "--red-local-ip",
                             "192.168.20.10", "--red-ip", "239.20.0.1"});
    auto result = parseOptions(args);
    REQUIRE(result.options);
    REQUIRE(result.options->redundant);
    CHECK(result.options->redundant->udpPort == 20010);
    CHECK(result.options->redundant->port == "0000:af:00.1");
}

TEST_CASE("Redundanter Pfad auf demselben Port ist ein Fehler") {
    auto args = baseArgs();
    args.insert(args.end(),
                {"--red-port", "0000:af:00.0", "--red-local-ip", "192.168.20.10", "--red-ip", "239.20.0.1"});
    CHECK(contains(parseOptions(args).errors, "denselben Port"));
}

TEST_CASE("Quelladresse bei Unicast ergibt einen Hinweis") {
    auto result = parseOptions({"st20-timing", "--port", "0000:af:00.0", "--local-ip", "192.168.10.10",
                                "--ip", "192.168.10.20", "--source-ip", "192.168.10.30"});
    REQUIRE(result.options);
    CHECK(contains(result.warnings, "nur bei Multicast"));
}

TEST_CASE("Adressprüfungen") {
    CHECK(isPciAddress("0000:af:00.0"));
    CHECK(isPciAddress("0000:AF:01.7"));
    CHECK_FALSE(isPciAddress("0000:af:00.8"));
    CHECK_FALSE(isPciAddress("kernel:eth0"));
    CHECK(isKernelPort("kernel:eth0"));
    CHECK_FALSE(isKernelPort("kernel:"));
    CHECK(isIpv4Address("10.0.0.1"));
    CHECK_FALSE(isIpv4Address("10.0.0"));
    CHECK(isMulticastIpv4("239.255.0.1"));
    CHECK(isMulticastIpv4("224.0.0.1"));
    CHECK_FALSE(isMulticastIpv4("192.168.0.1"));
}
