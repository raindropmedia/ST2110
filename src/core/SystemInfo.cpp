#include "core/SystemInfo.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>

#ifdef __linux__
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace st2110 {

using nlohmann::json;

namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// Schnittstellennamen aus sysfs: nur harmlose Zeichen (sie landen als Argument in ethtool).
bool validInterfaceName(const std::string& name) {
    if (name.empty() || name.size() > 15 || name[0] == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == '-' || c == ':';
    });
}

bool validPci(const std::string& pci) {
    // dddd:bb:ss.f
    if (pci.size() != 12 || pci[4] != ':' || pci[7] != ':' || pci[10] != '.') {
        return false;
    }
    return std::all_of(pci.begin(), pci.end(),
                       [](unsigned char c) { return std::isxdigit(c) || c == ':' || c == '.'; });
}

std::string readTrimmed(SystemProbe& probe, const std::string& path) {
    auto text = probe.readFile(path);
    return text ? trim(*text) : std::string{};
}

json keyValuesToJson(const std::vector<std::pair<std::string, std::string>>& items) {
    json out = json::array();
    for (const auto& [k, v] : items) {
        out.push_back(json{{"key", k}, {"value", v}});
    }
    return out;
}

std::string firstLine(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

std::string valueOf(const std::vector<std::pair<std::string, std::string>>& items, const std::string& key) {
    for (const auto& [k, v] : items) {
        if (k == key) {
            return v;
        }
    }
    return {};
}

// Bekannte Intel-Ethernet-Kennungen (nur sicher bekannte; sonst bleibt der Name leer).
std::string knownModel(const std::string& vendor, const std::string& device) {
    if (vendor == "0x8086" && device == "0x12de") {
        return "Intel Ethernet E830-L";
    }
    return {};
}

json pcieInfo(SystemProbe& probe, const std::string& dir) {
    return json{{"currentSpeed", readTrimmed(probe, dir + "/current_link_speed")},
                {"currentWidth", readTrimmed(probe, dir + "/current_link_width")},
                {"maxSpeed", readTrimmed(probe, dir + "/max_link_speed")},
                {"maxWidth", readTrimmed(probe, dir + "/max_link_width")}};
}

json nicInfo(SystemProbe& probe, const SystemOptions& options, const std::string& pci,
             const std::string& role) {
    const std::string dir = "/sys/bus/pci/devices/" + pci;
    json nic;
    nic["pci"] = pci;
    nic["role"] = role;
    const std::string vendor = readTrimmed(probe, dir + "/vendor");
    const std::string device = readTrimmed(probe, dir + "/device");
    nic["vendorId"] = vendor;
    nic["deviceId"] = device;
    nic["subsystemVendorId"] = readTrimmed(probe, dir + "/subsystem_vendor");
    nic["subsystemDeviceId"] = readTrimmed(probe, dir + "/subsystem_device");
    nic["model"] = knownModel(vendor, device);
    if (auto link = probe.readLink(dir + "/driver")) {
        const auto slash = link->find_last_of('/');
        nic["driver"] = slash == std::string::npos ? *link : link->substr(slash + 1);
    } else {
        nic["driver"] = "";
    }
    const std::string numa = readTrimmed(probe, dir + "/numa_node");
    nic["numaNode"] = numa.empty() ? json(nullptr) : json(std::atoi(numa.c_str()));
    if (auto group = probe.readLink(dir + "/iommu_group")) {
        const auto slash = group->find_last_of('/');
        nic["iommuGroup"] = slash == std::string::npos ? *group : group->substr(slash + 1);
    } else {
        nic["iommuGroup"] = "";
    }
    nic["pcie"] = pcieInfo(probe, dir);

    // Kernel-Schnittstelle (nur solange der Port am Kernel-Treiber hängt).
    std::string ifname;
    for (const auto& name : probe.listDir(dir + "/net")) {
        if (validInterfaceName(name)) {
            ifname = name;
            break;
        }
    }
    nic["interface"] = ifname;
    nic["management"] = !ifname.empty() && ifname == options.managementInterface;

    if (!ifname.empty()) {
        nic["live"] = true;
        const std::string netdir = "/sys/class/net/" + ifname;
        nic["mac"] = readTrimmed(probe, netdir + "/address");
        nic["mtu"] = readTrimmed(probe, netdir + "/mtu");
        nic["operstate"] = readTrimmed(probe, netdir + "/operstate");
        const auto drv = probe.run({"ethtool", "-i", ifname});
        const auto base = probe.run({"ethtool", ifname});
        const auto mod = probe.run({"ethtool", "-m", ifname});
        nic["ethtoolAvailable"] = drv.started;
        nic["driverInfo"] = keyValuesToJson(parseKeyValueLines(drv.output));
        nic["linkInfo"] = keyValuesToJson(parseKeyValueLines(base.output));
        nic["module"] = parseModuleInfo(mod.output, mod.started && mod.status == 0);
        if (!mod.started) {
            nic["module"]["message"] = "ethtool ist nicht installiert (apt install ethtool).";
        }
        return nic;
    }

    // Keine Kernel-Schnittstelle: Werte aus der Momentaufnahme vom Binden an vfio-pci.
    nic["live"] = false;
    nic["mac"] = "";
    nic["mtu"] = "";
    nic["operstate"] = "";
    nic["ethtoolAvailable"] = true;
    nic["driverInfo"] = json::array();
    nic["linkInfo"] = json::array();
    nic["module"] =
        json{{"present", false},
             {"message", "Port hängt nicht am Kernel-Treiber und es liegt keine Momentaufnahme vor."},
             {"identity", json::array()},
             {"dom", json::array()},
             {"alarms", json::array()}};
    nic["snapshotTime"] = nullptr;
    if (validPci(pci)) {
        if (auto text = probe.readFile(options.snapshotDir + "/" + pci + ".txt")) {
            std::string ethtoolI, ethtool, ethtoolM, mac, time;
            bool moduleOk = false;
            for (const auto& [name, body] : parseSnapshotSections(*text)) {
                if (name == "time") {
                    time = trim(body);
                } else if (name == "mac") {
                    mac = trim(body);
                } else if (name == "ethtool-i") {
                    ethtoolI = body;
                } else if (name == "ethtool") {
                    ethtool = body;
                } else if (name == "ethtool-m") {
                    ethtoolM = body;
                } else if (name == "ethtool-m-status") {
                    moduleOk = trim(body) == "0";
                }
            }
            nic["mac"] = mac;
            nic["driverInfo"] = keyValuesToJson(parseKeyValueLines(ethtoolI));
            nic["linkInfo"] = keyValuesToJson(parseKeyValueLines(ethtool));
            nic["module"] = parseModuleInfo(ethtoolM, moduleOk);
            nic["snapshotTime"] = time.empty() ? json(nullptr) : json(std::atoll(time.c_str()));
        }
    }
    return nic;
}

json hostInfo(SystemProbe& probe, const SystemOptions& options) {
    json host;
    host["hostname"] = readTrimmed(probe, "/proc/sys/kernel/hostname");
    host["kernel"] = readTrimmed(probe, "/proc/sys/kernel/osrelease");
    host["cmdline"] = readTrimmed(probe, "/proc/cmdline");
    host["managementInterface"] = options.managementInterface;
    const std::string up = readTrimmed(probe, "/proc/uptime");
    host["uptimeSec"] = up.empty() ? json(nullptr) : json(std::atof(up.c_str()));

    std::string model;
    int logical = 0;
    if (auto cpu = probe.readFile("/proc/cpuinfo")) {
        std::istringstream in(*cpu);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("processor", 0) == 0) {
                ++logical;
            } else if (line.rfind("model name", 0) == 0 && model.empty()) {
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    model = trim(line.substr(colon + 1));
                }
            }
        }
    }
    host["cpuModel"] = model;
    host["cpuThreads"] = logical;
    host["cpuGovernor"] = readTrimmed(probe, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");

    long total = -1;
    long freeCount = -1;
    long sizeKb = -1;
    if (auto mem = probe.readFile("/proc/meminfo")) {
        std::istringstream in(*mem);
        std::string line;
        auto number = [](const std::string& l) { return std::atol(l.substr(l.find(':') + 1).c_str()); };
        while (std::getline(in, line)) {
            if (line.rfind("HugePages_Total:", 0) == 0) {
                total = number(line);
            } else if (line.rfind("HugePages_Free:", 0) == 0) {
                freeCount = number(line);
            } else if (line.rfind("Hugepagesize:", 0) == 0) {
                sizeKb = number(line);
            }
        }
    }
    host["hugepages"] = json{{"total", total < 0 ? json(nullptr) : json(total)},
                             {"free", freeCount < 0 ? json(nullptr) : json(freeCount)},
                             {"sizeKb", sizeKb < 0 ? json(nullptr) : json(sizeKb)}};
    host["iommuGroups"] = static_cast<int>(probe.listDir("/sys/kernel/iommu_groups").size());
    return host;
}

} // namespace

std::vector<std::pair<std::string, std::string>> parseKeyValueLines(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (trim(line).empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            // Doppelpunkt trennt Schlüssel und Wert (ethtool rückt alles mit Tabulator ein). Zeilen wie
            // "Settings for enp1s0:" haben keinen Wert und werden übersprungen.
            const std::string key = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            if (!key.empty() && !(value.empty() && key.rfind("Settings for", 0) == 0)) {
                out.emplace_back(key, value);
            }
        } else if (!out.empty()) {
            // Fortsetzung ohne Doppelpunkt (ethtool listet Verbindungsarten untereinander)
            std::string& value = out.back().second;
            value += value.empty() ? trim(line) : "\n" + trim(line);
        }
    }
    return out;
}

json parseModuleInfo(const std::string& text, bool commandOk) {
    json out{{"present", false},
             {"message", ""},
             {"identity", json::array()},
             {"dom", json::array()},
             {"alarms", json::array()}};
    if (!commandOk) {
        out["message"] =
            firstLine(text).empty() ? "Kein Modul oder keine Modul-Daten lesbar." : firstLine(text);
        return out;
    }
    const auto items = parseKeyValueLines(text);
    if (items.empty()) {
        out["message"] = "Kein Modul oder keine Modul-Daten lesbar.";
        return out;
    }
    std::vector<std::pair<std::string, std::string>> identity;
    std::vector<std::pair<std::string, std::string>> dom;
    std::vector<std::pair<std::string, std::string>> alarms;
    for (const auto& item : items) {
        const std::string key = lower(item.first);
        if (contains(key, "alarm") || contains(key, "warning") || contains(key, "threshold")) {
            alarms.push_back(item);
        } else if (contains(key, "temperature") || contains(key, "voltage") ||
                   contains(key, "bias current") || contains(key, "output power") ||
                   contains(key, "optical power") || contains(key, "input power") ||
                   contains(key, "signal average")) {
            dom.push_back(item);
        } else {
            identity.push_back(item);
        }
    }
    out["present"] = true;
    out["identity"] = keyValuesToJson(identity);
    out["dom"] = keyValuesToJson(dom);
    out["alarms"] = keyValuesToJson(alarms);
    out["vendor"] = valueOf(identity, "Vendor name");
    out["partNumber"] = valueOf(identity, "Vendor PN");
    out["serial"] = valueOf(identity, "Vendor SN");
    return out;
}

std::vector<std::pair<std::string, std::string>> parseSnapshotSections(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("### ", 0) == 0) {
            out.emplace_back(trim(line.substr(4)), std::string{});
        } else if (!out.empty()) {
            out.back().second += line + "\n";
        }
    }
    return out;
}

json collectSystemInfo(SystemProbe& probe, const SystemOptions& options) {
    // Karten: benutzte Ports zuerst (in der angegebenen Reihenfolge), dann alle weiteren Intel-Ethernet-
    // Geräte (PCI-Klasse 0x02xx, Hersteller 0x8086).
    std::vector<SystemPort> ports;
    for (const auto& p : options.ports) {
        if (validPci(p.pci)) {
            ports.push_back(p);
        }
    }
    for (const auto& name : probe.listDir("/sys/bus/pci/devices")) {
        if (!validPci(name) ||
            std::any_of(ports.begin(), ports.end(), [&](const SystemPort& p) { return p.pci == name; })) {
            continue;
        }
        const std::string dir = "/sys/bus/pci/devices/" + name;
        if (readTrimmed(probe, dir + "/vendor") == "0x8086" &&
            readTrimmed(probe, dir + "/class").rfind("0x02", 0) == 0) {
            ports.push_back(SystemPort{name, ""});
        }
    }
    json nics = json::array();
    for (const auto& p : ports) {
        nics.push_back(nicInfo(probe, options, p.pci, p.role));
    }
    return json{{"available", true}, {"host", hostInfo(probe, options)}, {"nics", nics}};
}

#ifdef __linux__

namespace {

class LinuxProbe : public SystemProbe {
  public:
    std::optional<std::string> readFile(const std::string& path) override {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::optional<std::string> readLink(const std::string& path) override {
        char buffer[512];
        const ssize_t n = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
        if (n < 0) {
            return std::nullopt;
        }
        return std::string(buffer, static_cast<size_t>(n));
    }

    std::vector<std::string> listDir(const std::string& path) override {
        std::vector<std::string> names;
        DIR* dir = ::opendir(path.c_str());
        if (dir == nullptr) {
            return names;
        }
        while (const dirent* e = ::readdir(dir)) {
            const std::string name = e->d_name;
            if (name != "." && name != "..") {
                names.push_back(name);
            }
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());
        return names;
    }

    CommandResult run(const std::vector<std::string>& argv) override {
        CommandResult result;
        if (argv.empty()) {
            return result;
        }
        int pipefd[2];
        if (::pipe(pipefd) != 0) {
            return result;
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return result;
        }
        if (pid == 0) {
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
            }
            std::vector<char*> args;
            for (const auto& a : argv) {
                args.push_back(const_cast<char*>(a.c_str()));
            }
            args.push_back(nullptr);
            ::execvp(args[0], args.data());
            _exit(127);
        }
        ::close(pipefd[1]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        char buffer[4096];
        bool timedOut = false;
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) {
                timedOut = true;
                break;
            }
            struct pollfd pfd {
                pipefd[0], POLLIN, 0
            };
            const int r = ::poll(&pfd, 1, static_cast<int>(left));
            if (r <= 0) {
                timedOut = r == 0;
                if (r == 0) {
                    break;
                }
                continue;
            }
            const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            if (result.output.size() < 65536) {
                result.output.append(buffer, static_cast<size_t>(n));
            }
        }
        ::close(pipefd[0]);
        if (timedOut) {
            ::kill(pid, SIGKILL);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && result.output.empty()) {
            return result; // nicht gefunden
        }
        result.started = true;
        result.status = timedOut ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return result;
    }
};

} // namespace

std::unique_ptr<SystemProbe> makeSystemProbe() {
    return std::make_unique<LinuxProbe>();
}

#else

namespace {
class NullProbe : public SystemProbe {
  public:
    std::optional<std::string> readFile(const std::string&) override { return std::nullopt; }
    std::optional<std::string> readLink(const std::string&) override { return std::nullopt; }
    std::vector<std::string> listDir(const std::string&) override { return {}; }
    CommandResult run(const std::vector<std::string>&) override { return {}; }
};
} // namespace

std::unique_ptr<SystemProbe> makeSystemProbe() {
    return std::make_unique<NullProbe>();
}

#endif

} // namespace st2110
