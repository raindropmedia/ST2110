#pragma once

#include "core/Options.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace st2110 {

// SAP-Nachricht nach RFC 2974 (Ankündigung einer SDP-Sitzung, Multicast 239.255.255.255:9875).
struct SapMessage {
    bool deletion = false;
    uint16_t hash = 0;
    std::string origin; // Quelle der Ankündigung
    std::string sdp;
};

// Liefert false bei ungültigen, verschlüsselten oder komprimierten Nachrichten.
bool parseSapPacket(const uint8_t* data, size_t length, SapMessage& out, std::string& error);

// Angekündigte Sitzungen mit den daraus abgeleiteten Flows. Nicht threadsicher (der Hub sperrt).
class SapDirectory {
  public:
    struct Entry {
        std::string key; // Quelle/Hash
        std::string origin;
        std::string name;
        std::string sdp;
        std::vector<FlowConfig> flows;
        std::vector<std::string> errors;
        double firstSeen = 0;
        double lastSeen = 0;
        uint64_t announcements = 0;
    };

    void add(const SapMessage& message, double nowSec);
    // Ankündigungen ohne Wiederholung länger als maxAgeSec entfernen (RFC 2974: Vielfaches des Intervalls).
    void expire(double nowSec, double maxAgeSec = 600.0);
    const std::map<std::string, Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& key) const;

  private:
    std::map<std::string, Entry> entries_;
};

} // namespace st2110
