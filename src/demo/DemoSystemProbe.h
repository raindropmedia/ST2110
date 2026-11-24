#pragma once

#include "core/SystemInfo.h"

#include <map>

namespace st2110 {

// Feste Beispieldaten für --demo und die Tests: eine Karte E830-L mit zwei Ports, Port 1 am Kernel (SFP28 mit
// Diagnosewerten), Port 2 an vfio-pci (Momentaufnahme).
class DemoSystemProbe : public SystemProbe {
  public:
    DemoSystemProbe();
    std::optional<std::string> readFile(const std::string& path) override;
    std::optional<std::string> readLink(const std::string& path) override;
    std::vector<std::string> listDir(const std::string& path) override;
    CommandResult run(const std::vector<std::string>& argv) override;

    static SystemOptions options();

  private:
    std::map<std::string, std::string> files_;
    std::map<std::string, std::string> links_;
    std::map<std::string, std::vector<std::string>> dirs_;
    std::map<std::string, std::string> commands_;
};

} // namespace st2110
