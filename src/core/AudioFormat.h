#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace st2110 {

// Paketzeit nach ST 2110-30 (AES67): Dauer, die ein Paket an Abtastwerten trägt.
enum class PacketTime { P125us, P250us, P333us, P1ms, P4ms };

// Liest "0.125", "0.25", "0.333", "1", "4" (Millisekunden) bzw. "125us", "1ms".
std::optional<PacketTime> parsePacketTime(std::string_view text);
std::string packetTimeName(PacketTime ptime);
double packetTimeNs(PacketTime ptime);

struct AudioFormat {
    uint32_t sampleRate = 48000; // 48000 oder 96000
    uint16_t channels = 2;
    uint8_t bitDepth = 24; // 16 oder 24 (L16/L24)
    PacketTime ptime = PacketTime::P1ms;

    // Abtastwerte je Kanal und Paket (48 kHz, 1 ms: 48; 0,333 ms: 16).
    uint32_t samplesPerPacket() const;
    uint32_t payloadBytes() const { return samplesPerPacket() * channels * (bitDepth / 8u); }
    // z. B. "L24/48000/2 1ms"
    std::string name() const;
};

// Prüft Rate, Tiefe, Kanäle und Paketgröße; leer, wenn gültig.
std::string validateAudioFormat(const AudioFormat& format);

} // namespace st2110
