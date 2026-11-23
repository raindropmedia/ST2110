#include "core/PtpMonitor.h"

#include <doctest/doctest.h>

using namespace st2110;

TEST_CASE("PTP-Zustand: kein Sync, synchronisiert, eingerastet, Zeitüberschreitung") {
    PtpMonitor ptp;
    CHECK(ptp.state(0.0) == PtpState::NoSync);
    double t = 0;
    ptp.onSync(t, 50000, 37);
    CHECK(ptp.state(t) == PtpState::Syncing);
    for (int i = 0; i < 8; ++i) {
        t += 0.125;
        ptp.onSync(t, 40 - i * 10, 37);
    }
    CHECK(ptp.state(t) == PtpState::Locked);
    CHECK(ptp.utcOffset() == 37);
    ptp.onSync(t + 0.1, 5000, 37); // weit über der Hysterese: Lock verloren
    CHECK(ptp.state(t + 0.1) == PtpState::Syncing);
    CHECK(ptp.state(t + 10.0) == PtpState::NoSync);
    CHECK(ptp.syncCount() == 10);
    auto json = ptp.toJson(t, 60.0, 100);
    CHECK(json["syncs"] == 10);
    CHECK(json["histogram"]["count"] == 10);
    auto interval = ptp.takeInterval();
    CHECK(interval.count() == 10);
    CHECK(ptp.takeInterval().empty());
}

TEST_CASE("PTP-Zustand: Hysterese gegen Flackern bei leichter Streuung") {
    PtpMonitor ptp;
    double t = 0;
    for (int i = 0; i < 9; ++i) {
        ptp.onSync(t += 0.125, 800, 37);
    }
    REQUIRE(ptp.state(t) == PtpState::Locked);
    // Ausreißer knapp über der Einrastschwelle (500 ns Abweichung vom Mittelwert), aber unter der
    // Ausrastschwelle
    for (int64_t d : {1316, 1250, 400, 1100, 960}) {
        ptp.onSync(t += 0.125, d, 37);
        CHECK(ptp.state(t) == PtpState::Locked);
    }
    // Eine Abweichung ab 1000 ns rastet aus, danach braucht es acht ruhige Syncs
    ptp.onSync(t += 0.125, 2300, 37);
    CHECK(ptp.state(t) == PtpState::Syncing);
    for (int i = 0; i < 7; ++i) {
        ptp.onSync(t += 0.125, 900, 37);
        CHECK(ptp.state(t) == PtpState::Syncing);
    }
    ptp.onSync(t += 0.125, 900, 37);
    CHECK(ptp.state(t) == PtpState::Locked);
}

TEST_CASE("PTP: konstante Korrektur von 1,4 µs ist die Drift, kein Fehler") {
    // MTL stellt die Uhr je Sync zurück; bei 11 ppm Oszillatorfehler und 125 ms Sync-Abstand ist die
    // gemessene Korrektur jedes Mal rund 1410 ns (so beobachtet: 1374 bis 1433).
    PtpMonitor ptp;
    double t = 0;
    const int64_t values[] = {1400, 1410, 1385, 1433, 1374, 1420, 1405, 1415, 1398, 1412};
    for (int64_t v : values) {
        ptp.onSync(t += 0.125, v, 37);
    }
    CHECK(ptp.state(t) == PtpState::Locked);
    CHECK(ptp.driftNs() == doctest::Approx(1408).epsilon(0.01));
    const auto json = ptp.toJson(t, 60.0, 100);
    CHECK(json["driftNs"].get<double>() == doctest::Approx(1408).epsilon(0.01));
}

TEST_CASE("PTP: Wechselmuster durch doppeltes Nachstellen rastet nicht ein") {
    // Stellen zwei PTP-Instanzen denselben Timer nach, wechselt die Korrektur um die halbe Drift
    // (beobachtet: Mittel 706 ns bei Spannweite von etwa 0 bis 1440 ns).
    PtpMonitor ptp;
    double t = 0;
    for (int i = 0; i < 40; ++i) {
        ptp.onSync(t += 0.125, i % 2 == 0 ? 20 : 1400, 37);
        CHECK(ptp.state(t) != PtpState::Locked);
    }
    CHECK(ptp.driftNs() == doctest::Approx(710).epsilon(0.05));
}

TEST_CASE("PTP: Zeitsprung beim ersten Sync geht nicht in die Statistik") {
    PtpMonitor ptp;
    double t = 0;
    ptp.onSync(t += 0.125, 37000356475, 37); // UTC → TAI beim ersten Sync
    CHECK(ptp.steps() == 1);
    CHECK(ptp.lastStepNs() == 37000356475);
    CHECK(ptp.lastDeltaNs() == 37000356475);
    CHECK(ptp.state(t) == PtpState::Syncing);
    CHECK(ptp.totalDelta().empty());
    for (int i = 0; i < 8; ++i) {
        ptp.onSync(t += 0.125, 300, 37);
    }
    CHECK(ptp.state(t) == PtpState::Locked);
    CHECK(ptp.totalDelta().count() == 8);
    CHECK(ptp.totalDelta().max() == 300); // nicht 37 s
    const auto json = ptp.toJson(t, 60.0, 100);
    CHECK(json["steps"] == 1);
    CHECK(json["lastStepNs"] == 37000356475);
    CHECK(json["histogram"]["count"] == 8);
    // Ein späterer Sprung rastet aus und zählt weiter
    ptp.onSync(t += 0.125, -2000000, 37);
    CHECK(ptp.steps() == 2);
    CHECK(ptp.state(t) == PtpState::Syncing);
}

TEST_CASE("PTP-Zustand: nach einer Lücke wieder von vorn einrasten") {
    PtpMonitor ptp;
    double t = 0;
    for (int i = 0; i < 9; ++i) {
        ptp.onSync(t += 0.125, 100, 37);
    }
    REQUIRE(ptp.state(t) == PtpState::Locked);
    t += 10.0; // länger als die Zeitüberschreitung
    CHECK(ptp.state(t) == PtpState::NoSync);
    ptp.onSync(t, 100, 37);
    CHECK(ptp.state(t) == PtpState::Syncing);
}

TEST_CASE("Pfadversatz ordnet Bilder über RTP zu") {
    PathSkew skew;
    skew.add(false, 1000, 500000, 0.0);
    skew.add(false, 2800, 500100, 0.02);
    CHECK_FALSE(skew.hasValue());
    skew.add(true, 1000, 530000, 0.03);
    REQUIRE(skew.hasValue());
    CHECK(skew.last() == 30000);
    // R vor P: negativer Versatz
    skew.add(true, 4600, 490000, 0.04);
    skew.add(false, 4600, 500000, 0.05);
    CHECK(skew.last() == -10000);
    CHECK(skew.total().count() == 2);
    CHECK(skew.histogram().count() == 2);
}
