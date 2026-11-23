#include "core/PtpMonitor.h"

#include "core/Report.h"

#include <algorithm>
#include <cstdlib>

namespace st2110 {

const char* ptpStateName(PtpState state) {
    switch (state) {
    case PtpState::Locked:
        return "locked";
    case PtpState::Syncing:
        return "syncing";
    case PtpState::NoSync:
        break;
    }
    return "nosync";
}

PtpMonitor::PtpMonitor() : PtpMonitor(Settings{}) {}

PtpMonitor::PtpMonitor(Settings settings) : settings_(settings), histogram_(-2000.0, 2000.0, 200) {}

void PtpMonitor::onSync(double nowSec, int64_t deltaNs, int16_t utcOffset) {
    if (syncs_ > 0 && nowSec - lastSyncSec_ > settings_.timeoutSec) {
        // Nach einer Lücke von neuem einrasten
        locked_ = false;
        recent_.clear();
    }
    ++syncs_;
    utcOffset_ = utcOffset;
    lastDelta_ = deltaNs;
    lastSyncSec_ = nowSec;
    recent_.push_back(deltaNs);
    while (recent_.size() > settings_.lockSamples) {
        recent_.pop_front();
    }
    const auto magnitude = static_cast<double>(std::llabs(deltaNs));
    if (locked_) {
        if (magnitude >= std::max(settings_.unlockThresholdNs, settings_.lockThresholdNs)) {
            locked_ = false;
        }
    } else if (recent_.size() >= settings_.lockSamples) {
        locked_ = std::all_of(recent_.begin(), recent_.end(), [&](int64_t d) {
            return static_cast<double>(std::llabs(d)) < settings_.lockThresholdNs;
        });
    }
    total_.add(static_cast<double>(deltaNs));
    interval_.add(static_cast<double>(deltaNs));
    histogram_.add(static_cast<double>(deltaNs));
    series_.add(nowSec, static_cast<double>(deltaNs));
}

PtpState PtpMonitor::state(double nowSec) const {
    if (syncs_ == 0 || nowSec - lastSyncSec_ > settings_.timeoutSec) {
        return PtpState::NoSync;
    }
    return locked_ ? PtpState::Locked : PtpState::Syncing;
}

Range PtpMonitor::takeInterval() {
    Range out = interval_;
    interval_ = Range{};
    return out;
}

void PtpMonitor::resetTotals() {
    total_ = Range{};
    histogram_.clear();
}

nlohmann::json PtpMonitor::toJson(double nowSec, double windowSec, size_t maxPoints) const {
    return nlohmann::json{
        {"state", ptpStateName(state(nowSec))},
        {"syncs", syncs_},
        {"utcOffset", syncs_ ? nlohmann::json(utcOffset_) : nlohmann::json(nullptr)},
        {"lastDeltaNs", syncs_ ? nlohmann::json(lastDelta_) : nlohmann::json(nullptr)},
        {"sinceLastSyncSec", syncs_ ? nlohmann::json(nowSec - lastSyncSec_) : nlohmann::json(nullptr)},
        {"lockThresholdNs", settings_.lockThresholdNs},
        {"unlockThresholdNs", settings_.unlockThresholdNs},
        {"deltaNs", rangeToJson(total_)},
        {"histogram", histogramToJson(histogram_)},
        {"series", series_.toJson(windowSec, maxPoints)},
    };
}

PathSkew::PathSkew() : histogram_(-500000.0, 500000.0, 500) {}

void PathSkew::add(bool redundant, uint32_t rtpTimestamp, int64_t firstPacketNs, double nowSec) {
    if (redundant) {
        match(redundant_, primary_, true, rtpTimestamp, firstPacketNs, nowSec);
    } else {
        match(primary_, redundant_, false, rtpTimestamp, firstPacketNs, nowSec);
    }
}

void PathSkew::match(std::deque<Pending>& own, std::deque<Pending>& other, bool redundant, uint32_t rtp,
                     int64_t ns, double nowSec) {
    auto it = std::find_if(other.begin(), other.end(), [&](const Pending& p) { return p.rtp == rtp; });
    if (it == other.end()) {
        own.push_back({rtp, ns});
        while (own.size() > 16) {
            own.pop_front();
        }
        return;
    }
    const int64_t skew = redundant ? ns - it->ns : it->ns - ns;
    other.erase(other.begin(), it + 1);
    last_ = skew;
    hasLast_ = true;
    total_.add(static_cast<double>(skew));
    interval_.add(static_cast<double>(skew));
    histogram_.add(static_cast<double>(skew));
    series_.add(nowSec, static_cast<double>(skew));
}

Range PathSkew::takeInterval() {
    Range out = interval_;
    interval_ = Range{};
    return out;
}

void PathSkew::resetTotals() {
    total_ = Range{};
    histogram_.clear();
}

} // namespace st2110
