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
    ptp.onSync(t + 0.1, 5000, 37);
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
