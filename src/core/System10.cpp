#include "core/System10.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace st2110 {

using nlohmann::json;

namespace {

int rank(const std::string& v) {
    if (v == "fail") {
        return 3;
    }
    if (v == "warn") {
        return 2;
    }
    if (v == "ok") {
        return 1;
    }
    return 0; // na
}

std::string num(double v, int digits = 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

json check(const char* id, const char* label, std::string value, std::string limit, const char* verdict,
           std::string note = {}) {
    return json{{"id", id},
                {"label", label},
                {"value", std::move(value)},
                {"limit", std::move(limit)},
                {"verdict", verdict},
                {"note", std::move(note)}};
}

uint64_t sumPaths(const json& counters, const char* field) {
    uint64_t total = 0;
    if (counters.is_object() && counters.contains("paths")) {
        for (const auto& [key, p] : counters["paths"].items()) {
            if (p.contains(field) && p[field].is_number()) {
                total += p[field].get<uint64_t>();
            }
        }
    }
    return total;
}

} // namespace

json buildSystem10(const json& flow, const json& ptp, bool ptpEnabled, bool hwTimestamp) {
    json checks = json::array();
    const std::string kind = flow.value("kind", "");

    // 1. PTP: Alle Zeitstempel beziehen sich auf die PTP-Zeit (ST 2110-10 Abschnitt Zeitbezug).
    if (!ptpEnabled) {
        checks.push_back(
            check("ptp", "PTP-Zeitbezug", "aus", "eingerastet", "na",
                  "PTP ist abgeschaltet: Zeitwerte sind ohne Bezug zur Sendeuhr nicht bewertbar."));
    } else {
        const std::string state = ptp.value("state", "");
        std::string value = state == "locked"     ? "eingerastet"
                            : state == "unlocked" ? "nicht eingerastet"
                                                  : state;
        if (ptp.contains("lastDeltaNs") && ptp["lastDeltaNs"].is_number()) {
            value += " · " + num(ptp["lastDeltaNs"].get<double>()) + " ns";
        }
        const bool settling = ptp.value("settling", false);
        const char* verdict = state == "locked" ? (settling ? "warn" : "ok") : "fail";
        checks.push_back(check("ptp", "PTP-Zeitbezug", value, "eingerastet, Korrektur < 1000 ns", verdict,
                               settling ? "PTP schwingt noch ein." : ""));
    }

    // 2. RTP-Zeitstempel gegen PTP-Zeit, 3. Zeitstempelschritt (Grenzen von MTL, nur Video)
    if (kind == "video" && flow.contains("video") && flow["video"].is_object()) {
        const auto& video = flow["video"];
        const json& last = video.contains("paths") && video["paths"].contains("P")
                               ? video["paths"]["P"]["lastFrame"]
                               : json(nullptr);
        const json& pass = video.contains("pass") ? video["pass"] : json(nullptr);
        if (last.is_object() && pass.is_object()) {
            const double latency = last.value("latencyNs", 0);
            const double lo = pass.value("latencyMinNs", 0);
            const double hi = pass.value("latencyMaxNs", 0);
            const bool mismatch = std::abs(latency) >= 2147483000.0;
            const char* verdict = mismatch ? "fail" : (latency >= lo && latency <= hi) ? "ok" : "warn";
            checks.push_back(check(
                "rtp-ptp", "RTP-Zeitstempel gegen PTP-Zeit (Latenz)", num(latency / 1000.0, 1) + " µs",
                num(lo / 1000.0, 0) + " … " + num(hi / 1000.0, 0) + " µs", verdict,
                mismatch
                    ? "Zeitbasen von Sender und Empfänger weichen um Sekunden ab (PTP-Domäne, Zeitbezug)."
                    : "Abstand zwischen Sollzeit nach RTP-Zeitstempel und Ankunft des ersten Pakets."));
            const double delta = last.value("rtpTsDelta", 0);
            const double dlo = pass.value("rtpTsDeltaMin", 0);
            const double dhi = pass.value("rtpTsDeltaMax", 0);
            checks.push_back(check("rtp-step", "RTP-Zeitstempelschritt je Bild (90 kHz)", num(delta),
                                   num(dlo) + " … " + num(dhi),
                                   (delta >= dlo && delta <= dhi) ? "ok" : "fail",
                                   "Takt von 90 kHz: Schritt = 90000 / Bildrate."));
            const double offset = last.value("rtpOffset", 0);
            const double olo = pass.value("rtpOffsetMin", 0);
            const double ohi = pass.value("rtpOffsetMax", 0);
            checks.push_back(check("rtp-offset", "RTP-Offset (Zeitstempel gegen Sendezeit des Bildes)",
                                   num(offset) + " Takte", num(olo) + " … " + num(ohi) + " Takte",
                                   (offset >= olo && offset <= ohi) ? "ok" : "warn"));
        }
    } else if (kind == "audio" && flow.contains("audio") && flow["audio"].is_object()) {
        const auto& audio = flow["audio"];
        const json& last = audio.contains("paths") && audio["paths"].contains("P")
                               ? audio["paths"]["P"]["lastPeriod"]
                               : json(nullptr);
        if (last.is_object() && audio.contains("packetTimeUs")) {
            // DPVR ist bei Ton die Abweichung der Ankunft von der Sollzeit nach RTP-Zeitstempel.
            const double packet = audio["packetTimeUs"].get<double>();
            const double wide = audio.contains("pass") && audio["pass"].is_object()
                                    ? audio["pass"].value("dpvrMaxWideUs", 19.0 * packet)
                                    : 19.0 * packet;
            const double lo = last.value("dpvrMinUs", 0.0);
            const double hi = last.value("dpvrMaxUs", 0.0);
            const char* verdict = (lo >= -packet && hi <= wide) ? "ok" : "warn";
            checks.push_back(check(
                "rtp-ptp", "RTP-Zeitstempel gegen PTP-Zeit (DPVR)", num(lo, 0) + " … " + num(hi, 0) + " µs",
                num(-packet, 0) + " … " + num(wide, 0) + " µs", verdict,
                "Grenze: eine Paketzeit voraus, höchstens die wide-Grenze von ST 2110-30 nach."));
        }
    } else if (flow.contains(kind) && flow[kind].is_object() && flow[kind].contains("system10")) {
        // Flow-Arten mit eigener Zeitmessung (ST 2110-40, ST 2022-6) liefern ihre Zeitprüfungen selbst.
        for (const auto& c : flow[kind]["system10"]) {
            checks.push_back(c);
        }
    }

    // 4. Verluste und Reihenfolge (RTP-Sequenznummern, nach MTL-Zählern)
    const json& counters = flow.contains("counters") ? flow["counters"] : json(nullptr);
    if (counters.is_object()) {
        const uint64_t lost = std::max<uint64_t>(counters.value("lost", 0ULL), sumPaths(counters, "lost"));
        checks.push_back(check("loss", "Paketverlust (Sequenznummern)", std::to_string(lost), "0",
                               lost == 0 ? "ok" : "fail"));
        const uint64_t reordered = sumPaths(counters, "reordered");
        checks.push_back(check("reorder", "Vertauschte Pakete", std::to_string(reordered), "0",
                               reordered == 0 ? "ok" : "warn", "Paketreihenfolge im Netz geändert."));
        const uint64_t dup = sumPaths(counters, "duplicates");
        checks.push_back(check("dup", "Doppelte Pakete", std::to_string(dup), "0", dup == 0 ? "ok" : "warn"));
        const uint64_t wrongPt = counters.value("wrongPayloadType", 0ULL);
        const uint64_t wrongSsrc = counters.value("wrongSsrc", 0ULL);
        checks.push_back(check("pt-ssrc", "Pakete mit anderem Payload-Typ oder anderer SSRC (verworfen)",
                               std::to_string(wrongPt + wrongSsrc), "0",
                               (wrongPt + wrongSsrc) == 0 ? "ok" : "fail",
                               "Mehrere Sender im selben Multicast oder falscher Payload-Typ im SDP."));
    }

    const bool ownLoss =
        std::any_of(checks.begin(), checks.end(), [](const json& c) { return c["id"] == "loss"; });
    if (!counters.is_object() && !ownLoss) {
        checks.push_back(check("loss", "Paketverlust (Sequenznummern)", "keine Zähler", "0", "na",
                               "Die Quelle liefert keine Paketzähler."));
    }

    // 5. Payload-Typ im dynamischen Bereich (RFC 3551 / ST 2110-10: 96–127)
    if (flow.contains(kind) && flow[kind].is_object() && flow[kind].contains("payloadType") &&
        flow[kind]["payloadType"].is_number()) {
        const int pt = flow[kind]["payloadType"].get<int>();
        checks.push_back(check("pt", "Payload-Typ", std::to_string(pt), "96 … 127 (dynamisch)",
                               (pt >= 96 && pt <= 127) ? "ok" : "warn"));
    }

    // 6. Herkunft der Zeitstempel: Ohne Hardware-Zeitstempel sind alle Zeitwerte nur Richtwerte.
    const char* sources[] = {"video", "audio", "anc", "hbrmt"};
    uint64_t sw = 0;
    for (const char* s : sources) {
        if (flow.contains(s) && flow[s].is_object() && flow[s].contains("swTimestampPackets") &&
            flow[s]["swTimestampPackets"].is_number()) {
            sw += flow[s]["swTimestampPackets"].get<uint64_t>();
        }
    }
    if (!hwTimestamp || sw > 0) {
        checks.push_back(check("hw-ts", "Hardware-Zeitstempel",
                               sw ? std::to_string(sw) + " Pakete mit Software-Zeit" : "aus",
                               "alle Pakete mit Hardware-Zeit", "warn",
                               "Zeitwerte sind nur Richtwerte (Jitter des Betriebssystems)."));
    } else {
        checks.push_back(
            check("hw-ts", "Hardware-Zeitstempel", "alle Pakete", "alle Pakete mit Hardware-Zeit", "ok"));
    }

    int worst = 0;
    for (const auto& c : checks) {
        worst = std::max(worst, rank(c["verdict"].get<std::string>()));
    }
    const char* names[] = {"na", "ok", "warn", "fail"};
    return json{{"verdict", names[worst]}, {"checks", checks}};
}

} // namespace st2110
