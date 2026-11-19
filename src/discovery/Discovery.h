#pragma once

#include "core/MonitorHub.h"

#include <atomic>
#include <string>
#include <thread>

namespace st2110 {

// Flow-Erkennung auf einer Kernel-Schnittstelle (nur Linux):
// - liest alle Rahmen über einen AF_PACKET-Socket (Kopfdaten, je Rahmen 160 Byte) und meldet sie dem Hub,
// - empfängt SAP-Ankündigungen (239.255.255.255:9875) auf derselben Schnittstelle.
// Braucht CAP_NET_RAW (bzw. root). Die Schnittstelle sieht nur, was der Switch an sie liefert:
// Spiegelport, Port ohne IGMP-Snooping oder abonnierte Gruppen.
class Discovery {
  public:
    Discovery(MonitorHub& hub, std::string interfaceName, bool sap);
    ~Discovery();
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // Öffnet die Sockets und startet die Threads; false mit Fehlermeldung, wenn nichts davon geht.
    bool start(std::string& error);
    void stop();

  private:
    void captureLoop();
    void sapLoop();

    MonitorHub& hub_;
    std::string interface_;
    bool sap_;
    int captureSocket_ = -1;
    int sapSocket_ = -1;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> frames_{0};
    std::thread captureThread_;
    std::thread sapThread_;
};

} // namespace st2110
