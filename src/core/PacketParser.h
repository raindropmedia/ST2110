#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace st2110 {

// Kopfdaten eines mitgelesenen Pakets (IPv4/UDP, optional RTP und RFC 4175).
struct PacketInfo {
    uint32_t srcIp = 0; // Host-Byte-Reihenfolge
    uint32_t dstIp = 0;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    int vlan = -1;
    uint8_t dscp = 0;
    uint8_t ttl = 0;
    uint32_t wireBytes = 0;  // Länge des Ethernet-Rahmens bzw. IP-Pakets auf der Leitung
    uint16_t udpPayload = 0; // aus dem UDP-Kopf (unabhängig von der Erfassungslänge)

    bool rtp = false;
    uint8_t payloadType = 0;
    bool marker = false;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    uint16_t rtpPayload = 0;

    // RFC 4175 (ST 2110-20): erster Zeilenkopf und Nutzdaten des Pakets.
    bool rfc4175 = false;
    uint16_t row = 0;
    bool secondField = false;
    uint16_t rowOffset = 0;
    uint32_t videoBytes = 0;
    // Zeilensegmente des Pakets: Anzahl (mehrere = GPM, Segment setzt sich in der nächsten Zeile fort), Länge
    // des ersten sowie Zeile, Versatz und Länge des letzten Segments.
    uint8_t segments = 0;
    uint16_t firstLength = 0;
    uint16_t lastRow = 0;
    uint16_t lastOffset = 0;
    uint16_t lastLength = 0;
};

// Ethernet-Rahmen (mit optionalem VLAN-Tag). captured: vorhandene Bytes, wireLength: Länge auf der
// Leitung. Liefert false für alles außer IPv4/UDP ohne Fragmentierung.
bool parseEthernetFrame(const uint8_t* data, size_t captured, uint32_t wireLength, PacketInfo& out);
// IPv4-Paket ab dem IP-Kopf.
bool parseIpv4Packet(const uint8_t* data, size_t captured, PacketInfo& out);

std::string ipv4ToString(uint32_t hostOrderAddress);

} // namespace st2110
