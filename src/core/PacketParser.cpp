#include "core/PacketParser.h"

namespace st2110 {
namespace {

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// RTP-Kopf und, falls plausibel, RFC-4175-Zeilenköpfe auswerten.
void parseRtp(const uint8_t* p, size_t captured, PacketInfo& out) {
    const size_t payload = out.udpPayload;
    if (payload < 12 || captured < 12 || (p[0] >> 6) != 2) {
        return;
    }
    const uint8_t pt = p[1] & 0x7F;
    if (pt >= 72 && pt <= 76) {
        return; // RTCP
    }
    const size_t csrc = p[0] & 0x0F;
    size_t header = 12 + 4 * csrc;
    if ((p[0] & 0x10) != 0) { // Erweiterung
        if (captured < header + 4) {
            return;
        }
        header += 4 + 4 * static_cast<size_t>(be16(p + header + 2));
    }
    if (payload < header) {
        return;
    }
    out.rtp = true;
    out.marker = (p[1] & 0x80) != 0;
    out.payloadType = pt;
    out.sequence = be16(p + 2);
    out.timestamp = be32(p + 4);
    out.ssrc = be32(p + 8);
    out.rtpPayload = static_cast<uint16_t>(payload - header);

    // RFC 4175: 2 Byte erweiterte Sequenznummer, dann je Zeile 6 Byte (Länge, F|Zeile, C|Versatz).
    const uint8_t* q = p + header + 2;
    size_t avail = captured > header + 2 ? captured - header - 2 : 0;
    size_t used = 2;
    uint32_t bytes = 0;
    bool first = true;
    for (int i = 0; i < 8 && avail >= 6; ++i) {
        const uint16_t length = be16(q);
        const uint16_t fieldRow = be16(q + 2);
        const uint16_t contOffset = be16(q + 4);
        if (length == 0) {
            return;
        }
        if (first) {
            out.secondField = (fieldRow & 0x8000) != 0;
            out.row = fieldRow & 0x7FFF;
            out.rowOffset = contOffset & 0x7FFF;
            first = false;
        }
        bytes += length;
        used += 6;
        q += 6;
        avail -= 6;
        if ((contOffset & 0x8000) == 0) {
            break;
        }
    }
    // Plausibel, wenn Köpfe und Zeilendaten die Nutzlast (fast) genau füllen.
    if (!first && used + bytes <= out.rtpPayload && out.rtpPayload - (used + bytes) < 16 && out.row < 8704) {
        out.rfc4175 = true;
        out.videoBytes = bytes;
    }
}

} // namespace

bool parseIpv4Packet(const uint8_t* data, size_t captured, PacketInfo& out) {
    if (captured < 20 || (data[0] >> 4) != 4) {
        return false;
    }
    const size_t ihl = static_cast<size_t>(data[0] & 0x0F) * 4;
    if (ihl < 20 || captured < ihl + 8 || data[9] != 17) {
        return false; // nur UDP
    }
    const uint16_t fragment = be16(data + 6);
    if ((fragment & 0x1FFF) != 0 || (fragment & 0x2000) != 0) {
        return false; // fragmentiert
    }
    out.dscp = data[1] >> 2;
    out.ttl = data[8];
    out.srcIp = be32(data + 12);
    out.dstIp = be32(data + 16);
    if (out.wireBytes == 0) {
        out.wireBytes = be16(data + 2);
    }
    const uint8_t* udp = data + ihl;
    out.srcPort = be16(udp);
    out.dstPort = be16(udp + 2);
    const uint16_t udpLength = be16(udp + 4);
    out.udpPayload = udpLength >= 8 ? static_cast<uint16_t>(udpLength - 8) : 0;
    parseRtp(udp + 8, captured - ihl - 8, out);
    return true;
}

bool parseEthernetFrame(const uint8_t* data, size_t captured, uint32_t wireLength, PacketInfo& out) {
    out = PacketInfo{};
    if (captured < 14) {
        return false;
    }
    size_t offset = 12;
    uint16_t type = be16(data + offset);
    if (type == 0x8100 || type == 0x88A8) {
        if (captured < 18) {
            return false;
        }
        out.vlan = be16(data + 14) & 0x0FFF;
        offset = 16;
        type = be16(data + offset);
    }
    if (type != 0x0800) {
        return false;
    }
    out.wireBytes = wireLength;
    return parseIpv4Packet(data + offset + 2, captured - offset - 2, out);
}

std::string ipv4ToString(uint32_t a) {
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 0xFF) + "." +
           std::to_string((a >> 8) & 0xFF) + "." + std::to_string(a & 0xFF);
}

} // namespace st2110
