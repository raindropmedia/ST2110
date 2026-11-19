#include "core/Sdp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <sstream>

namespace st2110 {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, sep)) {
        if (sep != ' ' || !item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

template <typename T> bool toNumber(const std::string& s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && !s.empty();
}

// "c=IN IP4 239.1.1.1/64" -> "239.1.1.1"
std::string connectionAddress(const std::string& value) {
    auto parts = split(value, ' ');
    if (parts.size() < 3) {
        return {};
    }
    return parts[2].substr(0, parts[2].find('/'));
}

const SdpMedia* findByMid(const SdpSession& s, const std::string& mid) {
    for (const auto& m : s.media) {
        if (m.mid == mid) {
            return &m;
        }
    }
    return nullptr;
}

// Erstes Medium des Typs; mit DUP-Gruppe das zweite Medium als redundanter Pfad.
std::pair<const SdpMedia*, const SdpMedia*> pickMedia(const SdpSession& s, const std::string& type) {
    for (const auto& group : s.dupGroups) {
        if (group.size() >= 2) {
            const SdpMedia* a = findByMid(s, group[0]);
            const SdpMedia* b = findByMid(s, group[1]);
            if (a != nullptr && b != nullptr && a->type == type && b->type == type) {
                return {a, b};
            }
        }
    }
    for (const auto& m : s.media) {
        if (m.type == type) {
            return {&m, nullptr};
        }
    }
    return {nullptr, nullptr};
}

SdpFlow toFlow(const SdpMedia& m) {
    return SdpFlow{m.connectionIp, m.port, m.sourceIp};
}

std::string fmtpValue(const SdpMedia& m, const std::string& key) {
    auto it = m.fmtp.find(key);
    return it == m.fmtp.end() ? std::string{} : it->second;
}

} // namespace

std::optional<SdpSession> parseSdp(const std::string& text, std::string& error) {
    SdpSession session;
    std::string sessionConnection;
    std::istringstream in(text);
    std::string rawLine;
    SdpMedia* media = nullptr;
    bool sawVersion = false;

    while (std::getline(in, rawLine)) {
        const std::string line = trim(rawLine);
        if (line.size() < 2 || line[1] != '=') {
            continue;
        }
        const char kind = line[0];
        const std::string value = line.substr(2);
        switch (kind) {
        case 'v':
            sawVersion = true;
            break;
        case 's':
            session.name = value;
            break;
        case 'c': {
            const std::string ip = connectionAddress(value);
            if (media != nullptr) {
                media->connectionIp = ip;
            } else {
                sessionConnection = ip;
            }
            break;
        }
        case 'm': {
            auto parts = split(value, ' ');
            if (parts.size() < 4) {
                error = "Ungültige m=-Zeile: " + line;
                return std::nullopt;
            }
            SdpMedia m;
            m.type = parts[0];
            const std::string portText = parts[1].substr(0, parts[1].find('/'));
            if (!toNumber(portText, m.port)) {
                error = "Ungültiger Port in: " + line;
                return std::nullopt;
            }
            toNumber(parts[3], m.payloadType);
            session.media.push_back(m);
            media = &session.media.back();
            break;
        }
        case 'a': {
            const auto colon = value.find(':');
            const std::string name = value.substr(0, colon);
            const std::string arg =
                colon == std::string::npos ? std::string{} : trim(value.substr(colon + 1));
            if (name == "group") {
                auto parts = split(arg, ' ');
                if (!parts.empty() && parts[0] == "DUP") {
                    session.dupGroups.emplace_back(parts.begin() + 1, parts.end());
                }
            } else if (media == nullptr) {
                break;
            } else if (name == "rtpmap") {
                // a=rtpmap:96 raw/90000 bzw. a=rtpmap:97 L24/48000/2
                auto parts = split(arg, ' ');
                if (parts.size() >= 2) {
                    auto enc = split(parts[1], '/');
                    media->encoding = enc.empty() ? std::string{} : enc[0];
                    if (enc.size() > 1) {
                        toNumber(enc[1], media->clockRate);
                    }
                    if (enc.size() > 2) {
                        toNumber(enc[2], media->channels);
                    }
                }
            } else if (name == "fmtp") {
                const auto space = arg.find(' ');
                const std::string params = space == std::string::npos ? std::string{} : arg.substr(space + 1);
                for (const auto& item : split(params, ';')) {
                    const std::string kv = trim(item);
                    if (kv.empty()) {
                        continue;
                    }
                    const auto eq = kv.find('=');
                    if (eq == std::string::npos) {
                        media->fmtp[lower(kv)] = "";
                    } else {
                        media->fmtp[lower(trim(kv.substr(0, eq)))] = trim(kv.substr(eq + 1));
                    }
                }
            } else if (name == "ptime") {
                media->ptime = arg;
            } else if (name == "mid") {
                media->mid = arg;
            } else if (name == "source-filter") {
                // a=source-filter: incl IN IP4 <ziel> <quelle>
                auto parts = split(arg, ' ');
                if (parts.size() >= 5 && parts[0] == "incl") {
                    media->sourceIp = parts[4];
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (!sawVersion) {
        error = "Keine SDP-Beschreibung (v= fehlt).";
        return std::nullopt;
    }
    for (auto& m : session.media) {
        if (m.connectionIp.empty()) {
            m.connectionIp = sessionConnection;
        }
    }
    return session;
}

std::string VideoFormat::name() const {
    return std::to_string(width) + "x" + std::to_string(height) + (interlaced ? "i" : "p") +
           frameRateName(rate);
}

std::optional<VideoFormat> parseVideoFormat(const std::string& text) {
    static const std::regex kFormat(R"(^(\d+)x(\d+)([pi])([0-9.]+)$)");
    std::smatch m;
    if (!std::regex_match(text, m, kFormat)) {
        return std::nullopt;
    }
    VideoFormat f;
    if (!toNumber(m[1].str(), f.width) || !toNumber(m[2].str(), f.height)) {
        return std::nullopt;
    }
    f.interlaced = m[3].str() == "i";
    auto rate = parseFrameRate(m[4].str());
    if (!rate) {
        return std::nullopt;
    }
    f.rate = *rate;
    return f;
}

std::optional<FrameRate> parseExactFrameRate(const std::string& text) {
    static const std::map<std::string, FrameRate> kFractions{
        {"24000/1001", FrameRate::P23_98}, {"30000/1001", FrameRate::P29_97},
        {"60000/1001", FrameRate::P59_94}, {"120000/1001", FrameRate::P119_88},
        {"25/1", FrameRate::P25},          {"50/1", FrameRate::P50},
        {"24/1", FrameRate::P24},          {"30/1", FrameRate::P30},
        {"60/1", FrameRate::P60},          {"100/1", FrameRate::P100},
        {"120/1", FrameRate::P120},
    };
    if (auto it = kFractions.find(text); it != kFractions.end()) {
        return it->second;
    }
    return parseFrameRate(text);
}

std::optional<SdpVideo> sdpVideo(const SdpSession& session, std::string& error) {
    auto [p, r] = pickMedia(session, "video");
    if (p == nullptr) {
        return std::nullopt;
    }
    return sdpVideoFrom(*p, r, error);
}

std::optional<SdpVideo> sdpVideoFrom(const SdpMedia& media, const SdpMedia* redundantMedia,
                                     std::string& error) {
    const SdpMedia* p = &media;
    const SdpMedia* r = redundantMedia;
    SdpVideo v;
    v.primary = toFlow(*p);
    if (r != nullptr) {
        v.redundant = toFlow(*r);
    }
    v.payloadType = p->payloadType;
    if (lower(p->encoding) != "raw") {
        error = "Video: nur unkomprimiertes ST 2110-20 (raw) wird unterstützt, gefunden: " + p->encoding;
        return std::nullopt;
    }
    const std::string sampling = fmtpValue(*p, "sampling");
    const std::string depth = fmtpValue(*p, "depth");
    if (sampling != "YCbCr-4:2:2" || depth != "10") {
        error = "Video: nur YCbCr-4:2:2 mit 10 Bit wird unterstützt (SDP: sampling=" + sampling +
                ", depth=" + depth + ").";
        return std::nullopt;
    }
    if (!toNumber(fmtpValue(*p, "width"), v.format.width) ||
        !toNumber(fmtpValue(*p, "height"), v.format.height)) {
        error = "Video: width/height fehlen in a=fmtp.";
        return std::nullopt;
    }
    auto rate = parseExactFrameRate(fmtpValue(*p, "exactframerate"));
    if (!rate) {
        error = "Video: exactframerate fehlt oder wird nicht unterstützt (" +
                fmtpValue(*p, "exactframerate") + ").";
        return std::nullopt;
    }
    v.format.interlaced = p->fmtp.count("interlace") > 0;
    v.format.rate = *rate;
    if (v.format.interlaced) {
        // SDP nennt die Bildrate (25 bei 1080i50); MTL erwartet Halbbilder je Sekunde.
        switch (*rate) {
        case FrameRate::P25:
            v.format.rate = FrameRate::P50;
            break;
        case FrameRate::P29_97:
            v.format.rate = FrameRate::P59_94;
            break;
        case FrameRate::P30:
            v.format.rate = FrameRate::P60;
            break;
        default:
            error = "Video: Zeilensprung mit Bildrate " + frameRateName(*rate) + " wird nicht unterstützt.";
            return std::nullopt;
        }
    }
    return v;
}

std::optional<SdpAudio> sdpAudio(const SdpSession& session, std::string& error) {
    auto [p, r] = pickMedia(session, "audio");
    if (p == nullptr) {
        return std::nullopt;
    }
    return sdpAudioFrom(*p, r, error);
}

std::optional<SdpAudio> sdpAudioFrom(const SdpMedia& media, const SdpMedia* redundantMedia,
                                     std::string& error) {
    const SdpMedia* p = &media;
    const SdpMedia* r = redundantMedia;
    SdpAudio a;
    a.primary = toFlow(*p);
    if (r != nullptr) {
        a.redundant = toFlow(*r);
    }
    a.payloadType = p->payloadType;
    if (p->encoding == "L24") {
        a.format.bitDepth = 24;
    } else if (p->encoding == "L16") {
        a.format.bitDepth = 16;
    } else {
        error = "Ton: nur L16/L24 (ST 2110-30) wird unterstützt, gefunden: " + p->encoding;
        return std::nullopt;
    }
    a.format.sampleRate = p->clockRate;
    a.format.channels = p->channels == 0 ? 1 : p->channels;
    if (!p->ptime.empty()) {
        auto ptime = parsePacketTime(p->ptime);
        if (!ptime) {
            error = "Ton: Paketzeit " + p->ptime + " ms wird nicht unterstützt.";
            return std::nullopt;
        }
        a.format.ptime = *ptime;
    }
    if (auto problem = validateAudioFormat(a.format); !problem.empty()) {
        error = "Ton: " + problem;
        return std::nullopt;
    }
    return a;
}

std::vector<std::pair<const SdpMedia*, const SdpMedia*>> sdpMediaPairs(const SdpSession& session) {
    std::vector<std::pair<const SdpMedia*, const SdpMedia*>> out;
    std::vector<const SdpMedia*> secondary;
    for (const auto& group : session.dupGroups) {
        if (group.size() >= 2) {
            const SdpMedia* a = findByMid(session, group[0]);
            const SdpMedia* b = findByMid(session, group[1]);
            if (a != nullptr && b != nullptr && a->type == b->type) {
                secondary.push_back(b);
            }
        }
    }
    for (const auto& m : session.media) {
        if (std::find(secondary.begin(), secondary.end(), &m) != secondary.end()) {
            continue;
        }
        const SdpMedia* partner = nullptr;
        for (const auto& group : session.dupGroups) {
            if (group.size() >= 2 && !m.mid.empty() && group[0] == m.mid) {
                const SdpMedia* b = findByMid(session, group[1]);
                if (b != nullptr && b->type == m.type) {
                    partner = b;
                }
            }
        }
        out.emplace_back(&m, partner);
    }
    return out;
}

} // namespace st2110
