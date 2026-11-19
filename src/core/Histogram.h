#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <vector>

namespace st2110 {

// Histogramm mit gleich breiten Klassen im Bereich [lower, upper), dazu Unter- und Überlauf.
// Minimum, Maximum und Mittel werden exakt geführt, Perzentile aus den Klassen geschätzt.
class Histogram {
  public:
    Histogram() = default;
    Histogram(double lower, double upper, size_t bins);

    bool configured() const { return !bins_.empty(); }
    void add(double value, uint64_t count = 1);
    // Übernimmt die Werte eines Histogramms mit gleicher Einteilung; ein leeres, nicht eingerichtetes
    // Histogramm übernimmt die Einteilung des anderen. Andere Einteilungen werden ignoriert (false).
    bool merge(const Histogram& other);
    // Setzt die Zähler zurück, die Einteilung bleibt.
    void clear();

    double lower() const { return lower_; }
    double upper() const { return upper_; }
    double binWidth() const { return width_; }
    const std::vector<uint64_t>& bins() const { return bins_; }
    uint64_t underflow() const { return underflow_; }
    uint64_t overflow() const { return overflow_; }
    uint64_t count() const { return count_; }
    double min() const { return min_; }
    double max() const { return max_; }
    double mean() const { return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_); }
    // Perzentil p in [0, 1], aus den Klassen interpoliert; Unter-/Überlauf liefern lower/upper.
    double percentile(double p) const;

  private:
    double lower_ = 0;
    double upper_ = 0;
    double width_ = 0;
    std::vector<uint64_t> bins_;
    uint64_t underflow_ = 0;
    uint64_t overflow_ = 0;
    uint64_t count_ = 0;
    double sum_ = 0;
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
};

// {lower, upper, width, bins, underflow, overflow, count, min, max, mean, p50, p99}; null ohne Einteilung.
nlohmann::json histogramToJson(const Histogram& histogram);

} // namespace st2110
