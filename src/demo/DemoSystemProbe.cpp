#include "demo/DemoSystemProbe.h"

namespace st2110 {

namespace {

const char* kEthtoolI = R"(driver: ice
version: 6.18.0
firmware-version: 1.20 0x80014a2c 1.3700.0
expansion-rom-version: 
bus-info: 0000:01:00.0
supports-statistics: yes
supports-test: yes
supports-eeprom-access: yes
supports-register-dump: yes
supports-priv-flags: yes
)";

const char* kEthtool = R"(Settings for enp1s0f0np0:
	Supported ports: [ FIBRE ]
	Supported link modes:   1000baseX/Full
	                        10000baseSR/Full
	                        25000baseSR/Full
	Supported pause frame use: Symmetric
	Supports auto-negotiation: No
	Supported FEC modes: None RS BASER
	Advertised link modes:  25000baseSR/Full
	Advertised pause frame use: No
	Advertised auto-negotiation: No
	Advertised FEC modes: RS BASER
	Speed: 25000Mb/s
	Duplex: Full
	Auto-negotiation: off
	Port: FIBRE
	PHYAD: 0
	Transceiver: internal
	Supports Wake-on: g
	Wake-on: d
	Current message level: 0x00000007 (7)
	Link detected: yes
)";

const char* kModule = R"(	Identifier                                : 0x03 (SFP)
	Extended identifier                       : 0x04 (GBIC/SFP defined by 2-wire interface ID)
	Connector                                 : 0x07 (LC)
	Transceiver codes                         : 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00
	Transceiver type                          : 25G Ethernet: 25G BASE-SR
	Encoding                                  : 0x06 (64B/66B)
	BR, Nominal                               : 25500MBd
	Rate identifier                           : 0x00 (unspecified)
	Length (SMF,km)                           : 0km
	Length (OM3)                              : 70m
	Laser wavelength                          : 850.000nm
	Vendor name                               : FS
	Vendor OUI                                : 00:00:00
	Vendor PN                                 : SFP28-25GSR-85
	Vendor rev                                : A
	Vendor SN                                 : S2307123456
	Date code                                 : 230712
	Optical diagnostics support               : Yes
	Laser bias current                        : 6.700 mA
	Laser output power                        : 0.5740 mW / -2.41 dBm
	Receiver signal average optical power     : 0.5100 mW / -2.92 dBm
	Module temperature                        : 38.43 degrees C / 101.17 degrees F
	Module voltage                            : 3.2810 V
	Alarm/warning flags implemented           : Yes
	Laser bias current high alarm             : Off
	Laser bias current low alarm              : Off
	Laser output power high alarm             : Off
	Module temperature high alarm             : Off
	Module temperature high alarm threshold   : 80.00 degrees C / 176.00 degrees F
	Module temperature low alarm threshold    : -10.00 degrees C / 14.00 degrees F
	Laser output power high warning threshold : 1.4125 mW / 1.50 dBm
)";

const char* kSnapshot = R"(### time
1795500000
### mac
a0:36:bc:00:00:02
### ethtool-i
driver: ice
version: 6.18.0
firmware-version: 1.20 0x80014a2c 1.3700.0
bus-info: 0000:01:00.1
### ethtool
Settings for enp1s0f1np1:
	Supported ports: [ FIBRE ]
	Speed: 25000Mb/s
	Duplex: Full
	Port: FIBRE
	Link detected: yes
### ethtool-m-status
0
### ethtool-m
	Identifier                                : 0x03 (SFP)
	Connector                                 : 0x21 (Copper pigtail)
	Transceiver type                          : 25G Ethernet: 25G BASE-CR CA-S
	Length (copper or active cable)           : 1m
	Vendor name                               : Mellanox
	Vendor PN                                 : MCP2M00-A001
	Vendor SN                                 : MT2012VS12345
	Date code                                 : 200321
	Optical diagnostics support               : No
)";

} // namespace

DemoSystemProbe::DemoSystemProbe() {
    const std::string a = "/sys/bus/pci/devices/0000:01:00.0";
    const std::string b = "/sys/bus/pci/devices/0000:01:00.1";
    for (const auto& dir : {a, b}) {
        files_[dir + "/vendor"] = "0x8086\n";
        files_[dir + "/device"] = "0x12de\n";
        files_[dir + "/class"] = "0x020000\n";
        files_[dir + "/subsystem_vendor"] = "0x8086\n";
        files_[dir + "/subsystem_device"] = "0x4010\n";
        files_[dir + "/numa_node"] = "-1\n";
        files_[dir + "/current_link_speed"] = "16.0 GT/s PCIe\n";
        files_[dir + "/current_link_width"] = "8\n";
        files_[dir + "/max_link_speed"] = "16.0 GT/s PCIe\n";
        files_[dir + "/max_link_width"] = "8\n";
        links_[dir + "/iommu_group"] = "../../../kernel/iommu_groups/12";
    }
    links_[a + "/driver"] = "../../../bus/pci/drivers/ice";
    links_[b + "/driver"] = "../../../bus/pci/drivers/vfio-pci";
    dirs_["/sys/bus/pci/devices"] = {"0000:00:1f.6", "0000:01:00.0", "0000:01:00.1"};
    dirs_[a + "/net"] = {"enp1s0f0np0"};
    files_["/sys/class/net/enp1s0f0np0/address"] = "a0:36:bc:00:00:01\n";
    files_["/sys/class/net/enp1s0f0np0/mtu"] = "1500\n";
    files_["/sys/class/net/enp1s0f0np0/operstate"] = "up\n";
    files_["/sys/bus/pci/devices/0000:00:1f.6/vendor"] = "0x8086\n";
    files_["/sys/bus/pci/devices/0000:00:1f.6/class"] = "0x020000\n";
    files_["/sys/bus/pci/devices/0000:00:1f.6/device"] = "0x15a1\n";
    dirs_["/sys/bus/pci/devices/0000:00:1f.6/net"] = {"eno1"};
    links_["/sys/bus/pci/devices/0000:00:1f.6/driver"] = "../../../bus/pci/drivers/e1000e";
    files_["/sys/class/net/eno1/address"] = "00:11:22:33:44:55\n";
    files_["/sys/class/net/eno1/mtu"] = "1500\n";
    files_["/sys/class/net/eno1/operstate"] = "up\n";
    commands_["ethtool -i enp1s0f0np0"] = kEthtoolI;
    commands_["ethtool enp1s0f0np0"] = kEthtool;
    commands_["ethtool -m enp1s0f0np0"] = kModule;
    commands_["ethtool -i eno1"] =
        "driver: e1000e\nversion: 6.18.0\nfirmware-version: 0.5-4\nbus-info: 0000:00:1f.6\n";
    commands_["ethtool eno1"] =
        "Settings for eno1:\n\tSpeed: 1000Mb/s\n\tDuplex: Full\n\tPort: Twisted Pair\n\tLink detected: yes\n";
    files_["/run/st2110/nic/0000:01:00.1.txt"] = kSnapshot;
    files_["/proc/sys/kernel/hostname"] = "messplatz\n";
    files_["/proc/sys/kernel/osrelease"] = "6.18.0-demo\n";
    files_["/proc/cmdline"] =
        "BOOT_IMAGE=/vmlinuz root=/dev/sda2 ro quiet intel_iommu=on iommu=pt isolcpus=2-5\n";
    files_["/proc/uptime"] = "93784.12 600000.00\n";
    files_["/proc/cpuinfo"] =
        "processor\t: 0\nmodel name\t: Intel(R) Core(TM) i7-4790 CPU @ 3.60GHz\n\nprocessor\t: 1\nmodel "
        "name\t: Intel(R) Core(TM) i7-4790 CPU @ 3.60GHz\n";
    files_["/proc/meminfo"] =
        "HugePages_Total:    2048\nHugePages_Free:      900\nHugepagesize:       2048 kB\n";
    files_["/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"] = "performance\n";
    dirs_["/sys/kernel/iommu_groups"] = {"0", "1", "12"};
}

std::optional<std::string> DemoSystemProbe::readFile(const std::string& path) {
    const auto it = files_.find(path);
    return it == files_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::optional<std::string> DemoSystemProbe::readLink(const std::string& path) {
    const auto it = links_.find(path);
    return it == links_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::vector<std::string> DemoSystemProbe::listDir(const std::string& path) {
    const auto it = dirs_.find(path);
    return it == dirs_.end() ? std::vector<std::string>{} : it->second;
}

SystemProbe::CommandResult DemoSystemProbe::run(const std::vector<std::string>& argv) {
    std::string key;
    for (const auto& a : argv) {
        key += (key.empty() ? "" : " ") + a;
    }
    CommandResult r;
    r.started = true;
    const auto it = commands_.find(key);
    if (it == commands_.end()) {
        r.status = 1;
        r.output = "Cannot get module EEPROM information: Operation not supported\n";
    } else {
        r.status = 0;
        r.output = it->second;
    }
    return r;
}

SystemOptions DemoSystemProbe::options() {
    SystemOptions o;
    o.ports = {{"0000:01:00.0", "Empfang"}, {"0000:01:00.1", "Senden"}};
    o.managementInterface = "eno1";
    return o;
}

} // namespace st2110
