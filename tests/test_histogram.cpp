#include "core/Histogram.h"
#include "core/TimeSeries.h"

#include <doctest/doctest.h>

using namespace st2110;

TEST_CASE("Histogramm zählt Klassen, Unter- und Überlauf") {
    Histogram h(0.0, 10.0, 10);
    for (double v : {0.0, 0.5, 1.0, 9.99, 10.0, -1.0}) {
        h.add(v);
    }
    CHECK(h.count() == 6);
    CHECK(h.bins()[0] == 2);
    CHECK(h.bins()[1] == 1);
    CHECK(h.bins()[9] == 1);
    CHECK(h.underflow() == 1);
    CHECK(h.overflow() == 1);
    CHECK(h.min() == -1.0);
    CHECK(h.max() == 10.0);
}

TEST_CASE("Perzentile aus den Klassen") {
    Histogram h(0.0, 100.0, 100);
    for (int i = 0; i < 100; ++i) {
        h.add(i + 0.5);
    }
    CHECK(h.percentile(0.5) == doctest::Approx(50.0).epsilon(0.02));
    CHECK(h.percentile(0.99) == doctest::Approx(99.0).epsilon(0.02));
}

TEST_CASE("Zusammenführen nur bei gleicher Einteilung") {
    Histogram a(0.0, 10.0, 10);
    Histogram b(0.0, 10.0, 10);
    Histogram c(0.0, 20.0, 10);
    a.add(1);
    b.add(2);
    c.add(3);
    CHECK(a.merge(b));
    CHECK(a.count() == 2);
    CHECK_FALSE(a.merge(c));
    Histogram empty;
    CHECK(empty.merge(a));
    CHECK(empty.count() == 2);
    a.clear();
    CHECK(a.count() == 0);
    CHECK(a.configured());
    CHECK(histogramToJson(Histogram{}).is_null());
}

TEST_CASE("Zeitreihe begrenzt und verdichtet") {
    TimeSeries s(100);
    for (int i = 0; i < 150; ++i) {
        s.add(i * 0.1, i % 10 == 0 ? 100.0 : 1.0);
    }
    CHECK(s.size() == 100);
    auto all = s.toJson(1000.0, 1000);
    CHECK(all["t"].size() == 100);
    auto reduced = s.toJson(1000.0, 10);
    CHECK(reduced["t"].size() == 10);
    // Spitzen bleiben als Maximum der Gruppe erhalten.
    for (const auto& v : reduced["max"]) {
        CHECK(v.get<double>() == 100.0);
    }
    auto window = s.toJson(1.0, 1000);
    CHECK(window["t"].size() == 11);
}
