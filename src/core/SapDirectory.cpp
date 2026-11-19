#include "core/SapDirectory.h"

#include "core/PacketParser.h"

#include <cstring>

namespace st2110 {

bool parseSapPacket(const uint8_t* data, size_t length, SapMessage& out, std::string& error) {
    if (length < 8) {
        error = "SAP-Paket zu kurz";
        return false;
    }
    const uint8_t flags = data[0];
    if ((flags >> 5) != 1) {
        error = "SAP-Version nicht 1";
        return false;
    }
    const bool ipv6 = (flags & 0x10) != 0;
    if ((flags & 0x02) != 0 || (flags & 0x01) != 0) {
        error = "verschlüsselte oder komprimierte SAP-Nachricht";
        return false;
    }
    out.deletion = (flags & 0x04) != 0;
    const size_t authLength = static_cast<size_t>(data[1]) * 4;
    out.hash = static_cast<uint16_t>((data[2] << 8) | data[3]);
    size_t offset = 4;
    if (ipv6) {
        if (length < offset + 16) {
            error = "SAP-Paket zu kurz";
            return false;
        }
        out.origin = "IPv6";
        offset += 16;
    } else {
        const uint32_t origin = (static_cast<uint32_t>(data[4]) << 24) |
                                (static_cast<uint32_t>(data[5]) << 16) |
                                (static_cast<uint32_t>(data[6]) << 8) | data[7];
        out.origin = ipv4ToString(origin);
        offset += 4;
    }
    offset += authLength;
    if (offset > length) {
        error = "SAP-Paket zu kurz";
        return false;
    }
    std::string rest(reinterpret_cast<const char*>(data + offset), length - offset);
    // Optionaler Nutzlasttyp ("application/sdp\0"); fehlt er, beginnt die SDP mit "v=0".
    if (rest.rfind("v=", 0) != 0) {
        const auto nul = rest.find('\0');
        if (nul == std::string::npos) {
            error = "SAP ohne SDP";
            return false;
        }
        const std::string type = rest.substr(0, nul);
        if (type != "application/sdp") {
            error = "SAP-Nutzlast " + type + " wird nicht unterstützt";
            return false;
        }
        rest = rest.substr(nul + 1);
    }
    out.sdp = rest;
    return true;
}

void SapDirectory::add(const SapMessage& m, double now) {
    const std::string key = m.origin + "/" + std::to_string(m.hash);
    if (m.deletion) {
        entries_.erase(key);
        return;
    }
    auto [it, inserted] = entries_.try_emplace(key);
    Entry& e = it->second;
    if (inserted || e.sdp != m.sdp) {
        e.key = key;
        e.origin = m.origin;
        e.sdp = m.sdp;
        e.errors.clear();
        e.flows = flowsFromSdp(m.sdp, e.errors);
        e.name.clear();
        std::string error;
        if (auto session = parseSdp(m.sdp, error)) {
            e.name = session->name;
        }
        if (inserted) {
            e.firstSeen = now;
        }
    }
    e.lastSeen = now;
    e.announcements++;
}

void SapDirectory::expire(double now, double maxAgeSec) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.lastSeen > maxAgeSec) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

const SapDirectory::Entry* SapDirectory::find(const std::string& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

} // namespace st2110
