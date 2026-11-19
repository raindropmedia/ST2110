#include "core/TimeSeries.h"

#include <algorithm>
#include <cmath>

namespace st2110 {

void TimeSeries::add(double timeSec, double value) {
    if (std::isnan(value)) {
        return;
    }
    points_.push_back({timeSec, value});
    while (points_.size() > capacity_) {
        points_.pop_front();
    }
}

TimeSeries TimeSeries::tail(double windowSec) const {
    TimeSeries out(capacity_);
    if (points_.empty()) {
        return out;
    }
    const double from = points_.back().t - windowSec;
    auto first = std::find_if(points_.begin(), points_.end(), [&](const Point& p) { return p.t >= from; });
    out.points_.assign(first, points_.end());
    return out;
}

nlohmann::json TimeSeries::toJson(double windowSec, size_t maxPoints) const {
    nlohmann::json t = nlohmann::json::array();
    nlohmann::json mins = nlohmann::json::array();
    nlohmann::json maxs = nlohmann::json::array();
    if (!points_.empty() && maxPoints > 0) {
        const double from = points_.back().t - windowSec;
        auto first =
            std::find_if(points_.begin(), points_.end(), [&](const Point& p) { return p.t >= from; });
        const auto count = static_cast<size_t>(std::distance(first, points_.end()));
        const size_t group = std::max<size_t>(1, (count + maxPoints - 1) / maxPoints);
        for (auto it = first; it != points_.end();) {
            double lo = it->v;
            double hi = it->v;
            const double time = it->t;
            for (size_t i = 0; i < group && it != points_.end(); ++i, ++it) {
                lo = std::min(lo, it->v);
                hi = std::max(hi, it->v);
            }
            t.push_back(std::round(time * 1000.0) / 1000.0);
            mins.push_back(lo);
            maxs.push_back(hi);
        }
    }
    return nlohmann::json{{"t", t}, {"min", mins}, {"max", maxs}};
}

} // namespace st2110
