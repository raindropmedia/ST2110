#pragma once

#include "core/AudioFormat.h"
#include "core/FrameRate.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace st2110 {

// Ein Medienabschnitt (m=) einer SDP-Datei nach ST 2110-10/-20/-30.
struct SdpMedia {
    std::string type; // "video" | "audio" | ...
    uint16_t port = 0;
    int payloadType = -1;
    std::string connectionIp; // aus c= (Medium oder Sitzung), ohne /ttl
    std::string sourceIp;     // aus a=source-filter
    std::string encoding;     // aus a=rtpmap, z. B. "raw", "L24"
    uint32_t clockRate = 0;
    uint16_t channels = 0;
    std::map<std::string, std::string> fmtp; // Schlüssel klein geschrieben, Flags mit leerem Wert
    std::string ptime;
    std::string mid;
};

struct SdpSession {
    std::string name;
    std::vector<SdpMedia> media;
    std::vector<std::vector<std::string>> dupGroups; // a=group:DUP <mid> <mid>
};

// Liest eine SDP-Beschreibung; Fehler (deutsch) in error, dann leeres Ergebnis.
std::optional<SdpSession> parseSdp(const std::string& text, std::string& error);

// Videoformat aus fmtp und Bildrate ("50", "60000/1001").
struct VideoFormat {
    uint32_t width = 1920;
    uint32_t height = 1080;
    FrameRate rate = FrameRate::P50;
    bool interlaced = false;
    std::string name() const; // "1920x1080p50"
};

// "1920x1080p50", "1920x1080i50", "1280x720p59.94"
std::optional<VideoFormat> parseVideoFormat(const std::string& text);
std::optional<FrameRate> parseExactFrameRate(const std::string& text);

struct SdpFlow {
    std::string ip;
    uint16_t port = 0;
    std::string sourceIp;
};

struct SdpVideo {
    SdpFlow primary;
    std::optional<SdpFlow> redundant;
    VideoFormat format;
    int payloadType = 96;
};

struct SdpAudio {
    SdpFlow primary;
    std::optional<SdpFlow> redundant;
    AudioFormat format;
    int payloadType = 97;
};

// Erstes Video bzw. erster Ton der Sitzung; zwei Medien in einer DUP-Gruppe werden zu P und R.
// Liefert nichts, wenn das Medium fehlt; Fehler im Medium landen in error.
std::optional<SdpVideo> sdpVideo(const SdpSession& session, std::string& error);
std::optional<SdpAudio> sdpAudio(const SdpSession& session, std::string& error);

} // namespace st2110
