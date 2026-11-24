#include "core/SystemInfo.h"
#include "demo/DemoSystemProbe.h"

#include <doctest/doctest.h>

using namespace st2110;

TEST_CASE("System: Schlüssel-Wert-Zeilen mit Fortsetzungen") {
    const std::string text = "Settings for enp1s0f0np0:\n"
                             "\tSupported link modes:   1000baseX/Full\n"
                             "\t                        10000baseSR/Full\n"
                             "\tSpeed: 25000Mb/s\n"
                             "\tLink detected: yes\n";
    const auto items = parseKeyValueLines(text);
    REQUIRE(items.size() == 3);
    CHECK(items[0].first == "Supported link modes");
    CHECK(items[0].second == "1000baseX/Full\n10000baseSR/Full");
    CHECK(items[1].second == "25000Mb/s");
    CHECK(items[2].second == "yes");
}

TEST_CASE("System: Modulangaben werden in Kennung, Messwerte und Alarme sortiert") {
    DemoSystemProbe probe;
    const auto result = probe.run({"ethtool", "-m", "enp1s0f0np0"});
    const auto m = parseModuleInfo(result.output, true);
    CHECK(m["present"] == true);
    CHECK(m["vendor"] == "FS");
    CHECK(m["partNumber"] == "SFP28-25GSR-85");
    CHECK(m["serial"] == "S2307123456");
    bool temperature = false;
    for (const auto& item : m["dom"]) {
        temperature = temperature || item["key"] == "Module temperature";
    }
    CHECK(temperature);
    CHECK(m["alarms"].size() >= 5);
    // Schwellen gehören nicht zu den Messwerten
    for (const auto& item : m["dom"]) {
        CHECK(item["key"].get<std::string>().find("threshold") == std::string::npos);
    }
}

TEST_CASE("System: Fehler beim Lesen des Moduls") {
    const auto m = parseModuleInfo("Cannot get module EEPROM information: Operation not supported\n", false);
    CHECK(m["present"] == false);
    CHECK(m["message"] == "Cannot get module EEPROM information: Operation not supported");
    CHECK(parseModuleInfo("", true)["present"] == false);
}

TEST_CASE("System: Momentaufnahme in Abschnitte zerlegen") {
    const auto sections =
        parseSnapshotSections("### time\n123\n### mac\naa:bb\n### ethtool\nSpeed: 1\nDuplex: Full\n");
    REQUIRE(sections.size() == 3);
    CHECK(sections[0].first == "time");
    CHECK(sections[2].second == "Speed: 1\nDuplex: Full\n");
}

TEST_CASE("System: Karten aus Demo-Daten (Kernel-Port live, vfio-Port aus Momentaufnahme)") {
    DemoSystemProbe probe;
    const auto info = collectSystemInfo(probe, DemoSystemProbe::options());
    REQUIRE(info["nics"].size() == 3); // zwei benutzte Ports und die Onboard-Karte
    const auto& rx = info["nics"][0];
    CHECK(rx["pci"] == "0000:01:00.0");
    CHECK(rx["role"] == "Empfang");
    CHECK(rx["driver"] == "ice");
    CHECK(rx["live"] == true);
    CHECK(rx["interface"] == "enp1s0f0np0");
    CHECK(rx["model"] == "Intel Ethernet E830-L");
    CHECK(rx["pcie"]["currentWidth"] == "8");
    CHECK(rx["iommuGroup"] == "12");
    CHECK(rx["module"]["vendor"] == "FS");

    const auto& tx = info["nics"][1];
    CHECK(tx["driver"] == "vfio-pci");
    CHECK(tx["live"] == false);
    CHECK(tx["interface"] == "");
    CHECK(tx["mac"] == "a0:36:bc:00:00:02");
    CHECK(tx["snapshotTime"] == 1795500000);
    CHECK(tx["module"]["vendor"] == "Mellanox");

    const auto& onboard = info["nics"][2];
    CHECK(onboard["management"] == true);

    const auto& host = info["host"];
    CHECK(host["cpuThreads"] == 2);
    CHECK(host["hugepages"]["total"] == 2048);
    CHECK(host["iommuGroups"] == 3);
    CHECK(host["kernel"] == "6.18.0-demo");
}

TEST_CASE("System: ungültige PCI-Adressen und fehlendes ethtool") {
    DemoSystemProbe probe;
    SystemOptions options;
    options.ports = {{"../../etc", "x"}, {"0000:01:00.0", "Empfang"}};
    const auto info = collectSystemInfo(probe, options);
    bool badFound = false;
    for (const auto& n : info["nics"]) {
        badFound = badFound || n["pci"] == "../../etc";
    }
    CHECK_FALSE(badFound);
}
