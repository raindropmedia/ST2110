#include "core/HbrmtAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st2110 {

using nlohmann::json;

namespace {

constexpr size_t kRtpHeaderBytes = 12;
constexpr size_t kMediaPayloadBytes = 1376; // ST 2022-6 Abschnitt 6.2
constexpr double kVerdictWindowSec = 10.0;
constexpr size_t kSeriesCapacity = 6000;

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

std::string hex(unsigned v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02X", v);
    return buf;
}

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

json check(const char* id, const char* label, const std::string& value, const std::string& limit,
           const char* verdict, const std::string& note = {}) {
    return json{{"id", id},       {"label", label},     {"value", value},
                {"limit", limit}, {"verdict", verdict}, {"note", note}};
}

const char* levelName(HbrmtAnalyzer::Level level) {
    switch (level) {
    case HbrmtAnalyzer::Level::Ok:
        return "ok";
    case HbrmtAnalyzer::Level::Warn:
        return "warn";
    case HbrmtAnalyzer::Level::Fail:
        break;
    }
    return "failed";
}

} // namespace

bool parseHbrmtHeader(const uint8_t* data, size_t size, HbrmtHeader& out) {
    if (size < 8) {
        return false;
    }
    const uint32_t w0 = be32(data);
    const uint32_t w1 = be32(data + 4);
    out.ext = static_cast<uint8_t>(w0 >> 28);
    out.f = ((w0 >> 27) & 1u) != 0;
    out.vsid = static_cast<uint8_t>((w0 >> 24) & 7u);
    out.frCount = static_cast<uint8_t>((w0 >> 16) & 0xFFu);
    out.r = static_cast<uint8_t>((w0 >> 14) & 3u);
    out.s = static_cast<uint8_t>((w0 >> 12) & 3u);
    out.fec = static_cast<uint8_t>((w0 >> 9) & 7u);
    out.cf = static_cast<uint8_t>((w0 >> 5) & 0xFu);
    out.reserve = static_cast<uint8_t>(w0 & 0x1Fu);
    out.map = static_cast<uint8_t>(w1 >> 28);
    out.frame = static_cast<uint8_t>((w1 >> 20) & 0xFFu);
    out.frate = static_cast<uint8_t>((w1 >> 12) & 0xFFu);
    out.sample = static_cast<uint8_t>((w1 >> 8) & 0xFu);
    out.fmtReserve = static_cast<uint8_t>(w1 & 0xFFu);
    size_t length = 8;
    out.videoTimestamp = 0;
    if (out.cf > 0) {
        if (size < length + 4) {
            return false;
        }
        out.videoTimestamp = be32(data + length);
        length += 4;
    }
    length += static_cast<size_t>(out.ext) * 4;
    if (size < length) {
        return false;
    }
    out.length = length;
    return true;
}

std::string hbrmtFrameName(uint8_t frame) {
    switch (frame) {
    case 0x00:
        return "unbekannt";
    case 0x10:
        return "720×486 Zeilensprung (525)";
    case 0x11:
        return "720×576 Zeilensprung (625)";
    case 0x20:
        return "1920×1080 Zeilensprung";
    case 0x21:
        return "1920×1080 progressiv";
    case 0x22:
        return "1920×1080 progressiv, segmentiert (PsF)";
    case 0x23:
        return "2048×1080 progressiv";
    case 0x24:
        return "2048×1080 progressiv, segmentiert (PsF)";
    case 0x30:
        return "1280×720 progressiv";
    default:
        return "reserviert (" + hex(frame) + ")";
    }
}

std::string hbrmtRateName(uint8_t frate) {
    switch (frate) {
    case 0x00:
        return "unbekannt (2,970 GHz)";
    case 0x01:
        return "unbekannt (2,970/1,001 GHz)";
    case 0x02:
        return "unbekannt (1,485 GHz)";
    case 0x03:
        return "unbekannt (1,485/1,001 GHz)";
    case 0x04:
        return "unbekannt (0,270 GHz)";
    case 0x10:
        return "60 Hz";
    case 0x11:
        return "59,94 Hz";
    case 0x12:
        return "50 Hz";
    case 0x14:
        return "48 Hz";
    case 0x15:
        return "47,95 Hz";
    case 0x16:
        return "30 Hz";
    case 0x17:
        return "29,97 Hz";
    case 0x18:
        return "25 Hz";
    case 0x1A:
        return "24 Hz";
    case 0x1B:
        return "23,98 Hz";
    default:
        return "reserviert (" + hex(frate) + ")";
    }
}

std::string hbrmtSampleName(uint8_t sample) {
    switch (sample) {
    case 0x00:
        return "unbekannt";
    case 0x01:
        return "4:2:2, 10 Bit";
    case 0x02:
        return "4:4:4, 10 Bit";
    case 0x03:
        return "4:4:4:4, 10 Bit";
    case 0x05:
        return "4:2:2, 12 Bit";
    case 0x06:
        return "4:4:4, 12 Bit";
    case 0x07:
        return "4:4:4:4, 12 Bit";
    case 0x08:
        return "4:2:2:4, 12 Bit";
    default:
        return "reserviert (" + hex(sample) + ")";
    }
}

std::string hbrmtClockName(uint8_t cf) {
    switch (cf) {
    case 0:
        return "kein Zeitstempel";
    case 1:
        return "27 MHz";
    case 2:
        return "148,5 MHz";
    case 3:
        return "148,5/1,001 MHz";
    case 4:
        return "297 MHz";
    case 5:
        return "297/1,001 MHz";
    default:
        return "reserviert (" + std::to_string(cf) + ")";
    }
}

double hbrmtClockHz(uint8_t cf) {
    switch (cf) {
    case 1:
        return 27e6;
    case 2:
        return 148.5e6;
    case 3:
        return 148.5e6 / 1.001;
    case 4:
        return 297e6;
    case 5:
        return 297e6 / 1.001;
    default:
        return 0;
    }
}

double hbrmtFrameRateHz(uint8_t frate) {
    switch (frate) {
    case 0x10:
        return 60.0;
    case 0x11:
        return 60.0 / 1.001;
    case 0x12:
        return 50.0;
    case 0x14:
        return 48.0;
    case 0x15:
        return 48.0 / 1.001;
    case 0x16:
        return 30.0;
    case 0x17:
        return 30.0 / 1.001;
    case 0x18:
        return 25.0;
    case 0x1A:
        return 24.0;
    case 0x1B:
        return 24.0 / 1.001;
    default:
        return 0;
    }
}

uint32_t hbrmtExpectedDatagrams(uint8_t frame, uint8_t frate, uint8_t sample) {
    // Samples je Zeile (PL) und Zeilen je Bild (LF) aus den Rasternormen; das Bild ist der volle Rahmen.
    uint32_t pl = 0;
    uint32_t lf = 0;
    switch (frame) {
    case 0x20:
    case 0x21:
    case 0x22: // 1920×1080: ST 274
        lf = 1125;
        switch (frate) {
        case 0x10:
        case 0x11:
        case 0x16:
        case 0x17:
            pl = 2200;
            break;
        case 0x12:
        case 0x18:
            pl = 2640;
            break;
        case 0x14:
        case 0x15:
        case 0x1A:
        case 0x1B:
            pl = 2750;
            break;
        default:
            return 0;
        }
        break;
    case 0x30: // 1280×720: ST 296
        lf = 750;
        switch (frate) {
        case 0x10:
        case 0x11:
            pl = 1650;
            break;
        case 0x12:
            pl = 1980;
            break;
        case 0x16:
        case 0x17:
            pl = 3300;
            break;
        case 0x18:
            pl = 3960;
            break;
        case 0x14:
        case 0x15:
        case 0x1A:
        case 0x1B:
            pl = 4125;
            break;
        default:
            return 0;
        }
        break;
    case 0x10: // 525 Zeilen, 720×486: 858 Samples je Zeile
        pl = 858;
        lf = 525;
        break;
    case 0x11: // 625 Zeilen, 720×576: 864 Samples je Zeile
        pl = 864;
        lf = 625;
        break;
    default:
        return 0;
    }
    uint32_t bits = 0;
    uint32_t words = 2; // 4:2:2: zwei Wörter je Pixel; 4:4:4(:4): vier (Abschnitt 6.5)
    switch (sample) {
    case 0x01:
        bits = 10;
        break;
    case 0x05:
        bits = 12;
        break;
    case 0x02:
    case 0x03:
        bits = 10;
        words = 4;
        break;
    case 0x06:
    case 0x07:
        bits = 12;
        words = 4;
        break;
    default:
        return 0;
    }
    const uint64_t octetsPerLine = static_cast<uint64_t>(pl) * bits * words / 8;
    const uint64_t octetsPerFrame = octetsPerLine * lf;
    return static_cast<uint32_t>(octetsPerFrame / kMediaPayloadBytes + 1);
}

HbrmtAnalyzer::HbrmtAnalyzer() : mbitSeries_(kSeriesCapacity), dpfSeries_(kSeriesCapacity) {}

void HbrmtAnalyzer::reset() {
    *this = HbrmtAnalyzer();
}

HbrmtAnalyzer::Level HbrmtAnalyzer::verdict(double nowSec) const {
    if (nowSec - lastFailSec_ < kVerdictWindowSec) {
        return Level::Fail;
    }
    if (nowSec - lastWarnSec_ < kVerdictWindowSec) {
        return Level::Warn;
    }
    return Level::Ok;
}

std::optional<HbrmtAnalyzer::Note> HbrmtAnalyzer::addPacket(const uint8_t* rtp, size_t size, double nowSec) {
    std::string fail;
    std::string warn;
    ++packets_;
    bytes_ += size;
    ++secondPackets_;
    secondBytes_ += size;
    if (secondStart_ < 0) {
        secondStart_ = nowSec;
    }

    if (size < kRtpHeaderBytes) {
        ++badRtp_;
        fail = "RTP-Paket kürzer als der RTP-Header";
    } else {
        const uint8_t version = rtp[0] >> 6;
        const bool padding = ((rtp[0] >> 5) & 1) != 0;
        const bool extension = ((rtp[0] >> 4) & 1) != 0;
        const uint8_t cc = rtp[0] & 0x0F;
        const bool marker = (rtp[1] >> 7) != 0;
        const uint8_t pt = rtp[1] & 0x7F;
        const uint16_t seq = be16(rtp + 2);
        const uint32_t ts = be32(rtp + 4);
        const uint32_t ssrc = be32(rtp + 8);

        if (version != 2 || padding || extension || cc != 0) {
            ++badRtp_;
            fail = "RTP-Header verletzt ST 2022-6 (V=2, P=0, X=0, CC=0)";
        }
        if (pt != 98 && pt != 99) {
            ++badPt_;
            warn = "Payload-Typ " + std::to_string(pt) + " statt 98 (27-MHz-Takt)";
        }
        ptSeen_ = pt;
        if (haveSsrc_ && ssrc != ssrc_) {
            ++ssrcChanges_;
            warn = "SSRC gewechselt";
        }
        ssrc_ = ssrc;
        haveSsrc_ = true;

        // Sequenznummern
        if (haveSeq_) {
            const int16_t delta = static_cast<int16_t>(seq - nextSeq_);
            if (delta > 0) {
                lost_ += static_cast<uint64_t>(delta);
                fail = std::to_string(delta) + " Pakete fehlen (Sequenznummer)";
                nextSeq_ = static_cast<uint16_t>(seq + 1);
            } else if (delta < 0) {
                ++reordered_;
                if (lost_ > 0) {
                    --lost_; // nachträglich eingetroffen
                }
                warn = "Paket außer der Reihe";
            } else {
                nextSeq_ = static_cast<uint16_t>(seq + 1);
            }
        } else {
            nextSeq_ = static_cast<uint16_t>(seq + 1);
            haveSeq_ = true;
        }

        // Nutzdaten-Header
        HbrmtHeader h;
        const uint8_t* payload = rtp + kRtpHeaderBytes;
        const size_t payloadSize = size - kRtpHeaderBytes;
        if (!parseHbrmtHeader(payload, payloadSize, h)) {
            ++badHeader_;
            fail = "Nutzdaten-Header unvollständig";
        } else {
            if (!h.f) {
                ++badHeader_;
                fail = "F-Bit nicht gesetzt: ST 2022-6 verlangt das Videoformat im Header";
            }
            if (payloadSize - h.length != kMediaPayloadBytes) {
                ++badSize_;
                fail = "Nutzlast " + std::to_string(payloadSize - h.length) + " Byte statt " +
                       std::to_string(kMediaPayloadBytes);
            }
            if (haveHeader_ &&
                (h.map != header_.map || h.frame != header_.frame || h.frate != header_.frate ||
                 h.sample != header_.sample || h.cf != header_.cf || h.r != header_.r || h.s != header_.s ||
                 h.fec != header_.fec || h.vsid != header_.vsid)) {
                ++formatChanges_;
                warn = "Kopfangaben (Format, Takt oder Schutz) haben sich geändert";
            }
            // Bildaufbau: erstes Datagramm nach dem Marker beginnt ein Bild
            if (afterMarker_) {
                frameFirstTs_ = ts;
                frameFrCount_ = h.frCount;
                frameDatagrams_ = 0;
                if (havePrevFrCount_ && h.frCount != static_cast<uint8_t>(prevFrCount_ + 1)) {
                    ++frCountErrors_;
                    warn = "FRCount springt von " + std::to_string(prevFrCount_) + " auf " +
                           std::to_string(h.frCount);
                }
                prevFrCount_ = h.frCount;
                havePrevFrCount_ = true;
                if (haveLastFrameTs_) {
                    const int64_t step = static_cast<int32_t>(ts - lastFrameTs_);
                    lastStep_ = step;
                    if (!haveStep_) {
                        minStep_ = maxStep_ = step;
                        haveStep_ = true;
                    } else {
                        minStep_ = std::min(minStep_, step);
                        maxStep_ = std::max(maxStep_, step);
                    }
                    const double rate = hbrmtFrameRateHz(h.frate);
                    if (rate > 0) {
                        const double expected = 27e6 / rate;
                        if (std::fabs(static_cast<double>(step) - expected) > 2.0) {
                            ++stepErrors_;
                            warn = "RTP-Zeitstempelschritt " + std::to_string(step) + " statt " +
                                   fixed(expected, 0);
                        }
                    }
                }
                haveLastFrameTs_ = true;
                lastFrameTs_ = ts;
                afterMarker_ = false;
            } else if (h.frCount != frameFrCount_) {
                ++frCountErrors_;
                warn = "FRCount wechselt mitten im Bild";
            }
            ++frameDatagrams_;
            if (h.cf == 1) {
                const int32_t off = static_cast<int32_t>(h.videoTimestamp - ts);
                if (!haveVts_) {
                    vtsOffsetMin_ = vtsOffsetMax_ = off;
                    haveVts_ = true;
                } else {
                    vtsOffsetMin_ = std::min(vtsOffsetMin_, off);
                    vtsOffsetMax_ = std::max(vtsOffsetMax_, off);
                }
            }
            header_ = h;
            haveHeader_ = true;
            dpfExpected_ = hbrmtExpectedDatagrams(h.frame, h.frate, h.sample);
        }

        if (marker) {
            ++frames_;
            dpfLast_ = frameDatagrams_;
            dpfMin_ = std::min(dpfMin_, frameDatagrams_);
            dpfMax_ = std::max(dpfMax_, frameDatagrams_);
            dpfSeries_.add(nowSec, frameDatagrams_);
            if (dpfExpected_ > 0 && frameDatagrams_ != dpfExpected_) {
                ++dpfErrors_;
                warn = std::to_string(frameDatagrams_) + " Datagramme im Bild statt " +
                       std::to_string(dpfExpected_);
            }
            afterMarker_ = true;
        }
    }

    if (!fail.empty()) {
        lastFailSec_ = nowSec;
        lastCause_ = fail;
    } else if (!warn.empty()) {
        lastWarnSec_ = nowSec;
        lastCause_ = warn;
    }

    // Sekundenwerte und Meldung bei Urteilswechsel
    std::optional<Note> note;
    if (nowSec - secondStart_ >= 1.0) {
        const double dt = nowSec - secondStart_;
        lastPps_ = static_cast<double>(secondPackets_) / dt;
        lastMbit_ = static_cast<double>(secondBytes_) * 8.0 / dt / 1e6;
        mbitSeries_.add(nowSec, lastMbit_);
        secondStart_ = nowSec;
        secondPackets_ = 0;
        secondBytes_ = 0;
        const Level now = verdict(nowSec);
        if (now != lastNotified_) {
            note = Note{now, now == Level::Ok ? std::string("wieder in Ordnung") : lastCause_};
            lastNotified_ = now;
        }
    }
    return note;
}

json HbrmtAnalyzer::system10Checks() const {
    json checks = json::array();
    if (haveStep_) {
        const double rate = hbrmtFrameRateHz(header_.frate);
        if (rate > 0) {
            const double expected = 27e6 / rate;
            const bool ok = std::fabs(static_cast<double>(minStep_) - expected) <= 2.0 &&
                            std::fabs(static_cast<double>(maxStep_) - expected) <= 2.0;
            checks.push_back(check("rtp-step", "RTP-Zeitstempelschritt je Bild (27 MHz)",
                                   std::to_string(minStep_) + " … " + std::to_string(maxStep_),
                                   fixed(expected, 0) + " ± 2", ok ? "ok" : "fail",
                                   "Takt 27 MHz (Payload-Typ 98): Schritt = 27 000 000 / Bildrate."));
        } else {
            checks.push_back(check("rtp-step", "RTP-Zeitstempelschritt je Bild (27 MHz)",
                                   std::to_string(minStep_) + " … " + std::to_string(maxStep_),
                                   "Bildrate unbekannt (FRATE)", "na"));
        }
    }
    checks.push_back(check("loss", "Paketverlust (Sequenznummern)", std::to_string(lost_), "0",
                           lost_ == 0 ? "ok" : "fail"));
    checks.push_back(check("reorder", "Vertauschte Pakete", std::to_string(reordered_), "0",
                           reordered_ == 0 ? "ok" : "warn", "Paketreihenfolge im Netz geändert."));
    checks.push_back(check("pt-hbrmt", "Payload-Typ ST 2022-6", std::to_string(ptSeen_),
                           "98 (27 MHz) oder 99 (FEC)", (ptSeen_ == 98 || ptSeen_ == 99) ? "ok" : "warn"));
    if (haveVts_) {
        checks.push_back(
            check("vts", "Video-Zeitstempel gegen RTP-Zeitstempel (27 MHz)",
                  std::to_string(vtsOffsetMin_) + " … " + std::to_string(vtsOffsetMax_) + " Takte",
                  "konstant", vtsOffsetMin_ == vtsOffsetMax_ ? "ok" : "warn",
                  "Beide Zeitstempel laufen im selben Takt; ein wanderndes Verhältnis deutet auf getrennte "
                  "Uhren."));
    }
    return checks;
}

json HbrmtAnalyzer::toJson(double nowSec, double windowSec, size_t maxPoints) const {
    json header = nullptr;
    if (haveHeader_) {
        const auto& h = header_;
        header = json{{"ext", h.ext},
                      {"f", h.f},
                      {"vsid", h.vsid},
                      {"frCount", h.frCount},
                      {"r", h.r},
                      {"s", h.s},
                      {"fec", h.fec},
                      {"cf", h.cf},
                      {"map", h.map},
                      {"frame", h.frame},
                      {"frate", h.frate},
                      {"sample", h.sample},
                      {"frameName", hbrmtFrameName(h.frame)},
                      {"rateName", hbrmtRateName(h.frate)},
                      {"sampleName", hbrmtSampleName(h.sample)},
                      {"clockName", hbrmtClockName(h.cf)},
                      {"headerBytes", h.length},
                      {"videoTimestamp", h.cf > 0 ? json(h.videoTimestamp) : json(nullptr)}};
    }
    const std::string refNames[] = {"nicht synchronisiert", "reserviert", "UTC-synchron", "privater Takt"};
    const std::string fecNames[] = {"kein FEC", "Spalten-FEC (L)", "Spalten- und Zeilen-FEC (L und D)"};
    if (haveHeader_) {
        header["referenceName"] = refNames[header_.r & 3];
        header["fecName"] = header_.fec < 3 ? fecNames[header_.fec] : "reserviert";
        header["vsidName"] = header_.vsid == 0 ? "primär" : header_.vsid == 1 ? "Schutzstrom" : "reserviert";
    }
    return json{
        {"verdict", packets_ ? json(levelName(verdict(nowSec))) : json(nullptr)},
        {"cause", lastCause_},
        {"packets", packets_},
        {"bytes", bytes_},
        {"frames", frames_},
        {"lost", lost_},
        {"reordered", reordered_},
        {"badRtp", badRtp_},
        {"badPayloadType", badPt_},
        {"badSize", badSize_},
        {"badHeader", badHeader_},
        {"frCountErrors", frCountErrors_},
        {"formatChanges", formatChanges_},
        {"ssrcChanges", ssrcChanges_},
        {"stepErrors", stepErrors_},
        {"datagramErrors", dpfErrors_},
        {"header", header},
        {"datagramsPerFrame", json{{"last", dpfLast_},
                                   {"min", frames_ ? dpfMin_ : 0u},
                                   {"max", dpfMax_},
                                   {"expected", dpfExpected_ ? json(dpfExpected_) : json(nullptr)}}},
        {"rtpStep", json{{"last", haveStep_ ? json(lastStep_) : json(nullptr)},
                         {"min", haveStep_ ? json(minStep_) : json(nullptr)},
                         {"max", haveStep_ ? json(maxStep_) : json(nullptr)},
                         {"errors", stepErrors_}}},
        {"videoTimestampOffset",
         haveVts_ ? json{{"min", vtsOffsetMin_}, {"max", vtsOffsetMax_}} : json(nullptr)},
        {"rate", json{{"mbit", lastMbit_}, {"pps", lastPps_}}},
        {"series", json{{"mbit", mbitSeries_.toJson(windowSec, maxPoints)},
                        {"datagramsPerFrame", dpfSeries_.toJson(windowSec, maxPoints)}}},
        {"system10", system10Checks()},
    };
}

} // namespace st2110
