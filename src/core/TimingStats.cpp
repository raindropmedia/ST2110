#include "core/TimingStats.h"

#include <algorithm>

namespace st2110 {

const char* complianceName(Compliance compliance) {
    switch (compliance) {
    case Compliance::Narrow:
        return "narrow";
    case Compliance::Wide:
        return "wide";
    case Compliance::Failed:
        break;
    }
    return "failed";
}

void Range::add(double minValue, double maxValue, double avgValue) {
    min_ = std::min(min_, minValue);
    max_ = std::max(max_, maxValue);
    sum_ += avgValue;
    ++count_;
}

void Range::merge(const Range& other) {
    if (other.empty()) {
        return;
    }
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    sum_ += other.sum_;
    count_ += other.count_;
}

void TimingStats::merge(const TimingStats& other) {
    frames_ += other.frames_;
    packets_ += other.packets_;
    for (size_t i = 0; i < counts_.size(); ++i) {
        counts_[i] += other.counts_[i];
    }
    cinst_.merge(other.cinst_);
    vrx_.merge(other.vrx_);
    ipt_.merge(other.ipt_);
    fpt_.merge(other.fpt_);
    latency_.merge(other.latency_);
    rtpOffset_.merge(other.rtpOffset_);
    rtpTsDelta_.merge(other.rtpTsDelta_);
    for (const auto& [cause, count] : other.causes_) {
        addCause(cause, count);
    }
}

void TimingStats::addCause(const std::string& cause, uint64_t count) {
    auto it = causes_.find(cause);
    if (it != causes_.end()) {
        it->second += count;
    } else if (causes_.size() < kMaxCauses) {
        causes_.emplace(cause, count);
    } else {
        causes_[kOtherCauses] += count;
    }
}

void TimingStats::add(const FrameTiming& frame) {
    ++frames_;
    packets_ += frame.packets;
    const auto index = static_cast<size_t>(frame.compliance);
    if (index < counts_.size()) {
        ++counts_[index];
    }

    cinst_.add(frame.cinstMin, frame.cinstMax, frame.cinstAvg);
    vrx_.add(frame.vrxMin, frame.vrxMax, frame.vrxAvg);
    ipt_.add(frame.iptMinNs, frame.iptMaxNs, frame.iptAvgNs);
    fpt_.add(frame.fptNs);
    latency_.add(frame.latencyNs);
    rtpOffset_.add(frame.rtpOffset);
    rtpTsDelta_.add(frame.rtpTsDelta);

    if (frame.compliance != Compliance::Narrow && !frame.failedCause.empty()) {
        addCause(frame.failedCause, 1);
    }
}

Compliance TimingStats::worst() const {
    if (frames_ == 0 || count(Compliance::Failed) > 0) {
        return Compliance::Failed;
    }
    if (count(Compliance::Wide) > 0) {
        return Compliance::Wide;
    }
    return Compliance::Narrow;
}

} // namespace st2110
