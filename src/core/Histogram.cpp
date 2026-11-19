#include "core/Histogram.h"

#include <algorithm>
#include <cmath>

namespace st2110 {

Histogram::Histogram(double lower, double upper, size_t bins)
    : lower_(lower), upper_(upper), width_(bins > 0 && upper > lower ? (upper - lower) / bins : 0),
      bins_(width_ > 0 ? bins : 0, 0) {}

void Histogram::add(double value, uint64_t count) {
    if (!configured() || count == 0 || std::isnan(value)) {
        return;
    }
    if (value < lower_) {
        underflow_ += count;
    } else if (value >= upper_) {
        overflow_ += count;
    } else {
        auto index = static_cast<size_t>((value - lower_) / width_);
        bins_[std::min(index, bins_.size() - 1)] += count;
    }
    count_ += count;
    sum_ += value * static_cast<double>(count);
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
}

bool Histogram::merge(const Histogram& other) {
    if (!other.configured()) {
        return true;
    }
    if (!configured()) {
        *this = other;
        return true;
    }
    if (other.bins_.size() != bins_.size() || other.lower_ != lower_ || other.upper_ != upper_) {
        return false;
    }
    for (size_t i = 0; i < bins_.size(); ++i) {
        bins_[i] += other.bins_[i];
    }
    underflow_ += other.underflow_;
    overflow_ += other.overflow_;
    count_ += other.count_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    return true;
}

void Histogram::clear() {
    std::fill(bins_.begin(), bins_.end(), 0);
    underflow_ = overflow_ = count_ = 0;
    sum_ = 0;
    min_ = std::numeric_limits<double>::infinity();
    max_ = -std::numeric_limits<double>::infinity();
}

double Histogram::percentile(double p) const {
    if (count_ == 0) {
        return 0.0;
    }
    const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(count_);
    double seen = static_cast<double>(underflow_);
    if (target <= seen) {
        return lower_;
    }
    for (size_t i = 0; i < bins_.size(); ++i) {
        const double inBin = static_cast<double>(bins_[i]);
        if (seen + inBin >= target && inBin > 0) {
            const double fraction = (target - seen) / inBin;
            return lower_ + (static_cast<double>(i) + fraction) * width_;
        }
        seen += inBin;
    }
    return upper_;
}

nlohmann::json histogramToJson(const Histogram& h) {
    if (!h.configured()) {
        return nullptr;
    }
    const bool any = h.count() > 0;
    return nlohmann::json{
        {"lower", h.lower()},
        {"upper", h.upper()},
        {"width", h.binWidth()},
        {"bins", h.bins()},
        {"underflow", h.underflow()},
        {"overflow", h.overflow()},
        {"count", h.count()},
        {"min", any ? nlohmann::json(h.min()) : nlohmann::json(nullptr)},
        {"max", any ? nlohmann::json(h.max()) : nlohmann::json(nullptr)},
        {"mean", any ? nlohmann::json(h.mean()) : nlohmann::json(nullptr)},
        {"p50", any ? nlohmann::json(h.percentile(0.5)) : nlohmann::json(nullptr)},
        {"p99", any ? nlohmann::json(h.percentile(0.99)) : nlohmann::json(nullptr)},
    };
}

} // namespace st2110
