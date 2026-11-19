#pragma once

#include <cstddef>
#include <deque>
#include <nlohmann/json.hpp>

namespace st2110 {

// Zeitreihe mit begrenzter Länge: (Zeit in s seit Messbeginn, Wert). Ältere Punkte fallen heraus.
class TimeSeries {
  public:
    explicit TimeSeries(size_t capacity = 6000) : capacity_(capacity) {}

    void add(double timeSec, double value);
    void clear() { points_.clear(); }
    size_t size() const { return points_.size(); }
    bool empty() const { return points_.empty(); }

    struct Point {
        double t;
        double v;
    };
    const std::deque<Point>& points() const { return points_; }

    // Punkte der letzten windowSec Sekunden, höchstens maxPoints Gruppen. Bei mehr Punkten fasst die
    // Ausgabe Gruppen zusammen und gibt je Gruppe Minimum und Maximum aus (Spitzen bleiben sichtbar).
    // Format: {"t": [...], "min": [...], "max": [...]}.
    nlohmann::json toJson(double windowSec, size_t maxPoints) const;
    // Kopie mit den Punkten der letzten windowSec Sekunden (für Auswertungen außerhalb einer Sperre).
    TimeSeries tail(double windowSec) const;

  private:
    size_t capacity_;
    std::deque<Point> points_;
};

} // namespace st2110
