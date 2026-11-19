#include "mtl/St20TimingReceiver.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mtl_api.h>
#include <mutex>
#include <st20_api.h>
#include <st_pipeline_api.h>
#include <thread>

namespace st2110 {
namespace {

enum st_fps toMtlFps(FrameRate rate) {
    switch (rate) {
    case FrameRate::P23_98:
        return ST_FPS_P23_98;
    case FrameRate::P24:
        return ST_FPS_P24;
    case FrameRate::P25:
        return ST_FPS_P25;
    case FrameRate::P29_97:
        return ST_FPS_P29_97;
    case FrameRate::P30:
        return ST_FPS_P30;
    case FrameRate::P50:
        return ST_FPS_P50;
    case FrameRate::P59_94:
        return ST_FPS_P59_94;
    case FrameRate::P60:
        return ST_FPS_P60;
    case FrameRate::P100:
        return ST_FPS_P100;
    case FrameRate::P119_88:
        return ST_FPS_P119_88;
    case FrameRate::P120:
        return ST_FPS_P120;
    }
    return ST_FPS_MAX;
}

enum mtl_log_level toMtlLogLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return MTL_LOG_LEVEL_DEBUG;
    case LogLevel::Info:
        return MTL_LOG_LEVEL_INFO;
    case LogLevel::Notice:
        return MTL_LOG_LEVEL_NOTICE;
    case LogLevel::Warning:
        return MTL_LOG_LEVEL_WARNING;
    case LogLevel::Error:
        return MTL_LOG_LEVEL_ERR;
    }
    return MTL_LOG_LEVEL_INFO;
}

Compliance toCompliance(enum st_rx_tp_compliant value) {
    switch (value) {
    case ST_RX_TP_COMPLIANT_NARROW:
        return Compliance::Narrow;
    case ST_RX_TP_COMPLIANT_WIDE:
        return Compliance::Wide;
    default:
        return Compliance::Failed;
    }
}

FrameTiming toFrameTiming(const struct st20_rx_tp_meta& tp) {
    FrameTiming out;
    out.cinstMin = tp.cinst_min;
    out.cinstMax = tp.cinst_max;
    out.cinstAvg = tp.cinst_avg;
    out.vrxMin = tp.vrx_min;
    out.vrxMax = tp.vrx_max;
    out.vrxAvg = tp.vrx_avg;
    out.iptMinNs = tp.ipt_min;
    out.iptMaxNs = tp.ipt_max;
    out.iptAvgNs = tp.ipt_avg;
    out.fptNs = tp.fpt;
    out.latencyNs = tp.latency;
    out.rtpOffset = tp.rtp_offset;
    out.rtpTsDelta = tp.rtp_ts_delta;
    out.compliance = toCompliance(tp.compliant);
    out.failedCause.assign(tp.failed_cause, strnlen(tp.failed_cause, sizeof(tp.failed_cause)));
    out.packets = tp.pkts_cnt;
    return out;
}

PassCriteria toPassCriteria(const struct st20_rx_tp_pass& pass) {
    PassCriteria out;
    out.cinstMaxNarrow = pass.cinst_max_narrow;
    out.cinstMaxWide = pass.cinst_max_wide;
    out.vrxMaxNarrow = pass.vrx_max_narrow;
    out.vrxMaxWide = pass.vrx_max_wide;
    out.trOffsetNs = pass.tr_offset;
    out.latencyMaxNs = pass.latency_max;
    out.latencyMinNs = pass.latency_min;
    out.rtpOffsetMax = pass.rtp_offset_max;
    out.rtpOffsetMin = pass.rtp_offset_min;
    out.rtpTsDeltaMax = pass.rtp_ts_delta_max;
    out.rtpTsDeltaMin = pass.rtp_ts_delta_min;
    return out;
}

bool toIpv4(const std::string& text, uint8_t* out) {
    return inet_pton(AF_INET, text.c_str(), out) == 1;
}

} // namespace

struct St20TimingReceiver::Impl {
    Options options;
    std::string lcores; // muss für mtl_init_params::lcores gültig bleiben
    mtl_handle mtl = nullptr;
    st20p_rx_handle session = nullptr;
    std::thread worker;
    std::atomic<bool> stopping{false};

    mutable std::mutex mutex;
    Interval interval;
    std::optional<PassCriteria> criteria;

    ~Impl() {
        stopWorker();
        if (session != nullptr) {
            st20p_rx_free(session);
        }
        if (mtl != nullptr) {
            mtl_uninit(mtl);
        }
    }

    void stopWorker() {
        stopping = true;
        if (session != nullptr) {
            st20p_rx_wake_block(session);
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run() {
        while (!stopping) {
            struct st_frame* frame = st20p_rx_get_frame(session);
            if (frame == nullptr) {
                continue; // Zeitüberschreitung (Vorgabe 1 s) oder geweckt
            }
            consume(*frame);
            st20p_rx_put_frame(session, frame);
        }
    }

    void consume(const struct st_frame& frame) {
        std::optional<PassCriteria> newCriteria;
        if (!hasCriteria()) {
            struct st20_rx_tp_pass pass {};
            if (st20p_rx_timing_parser_critical(session, &pass) >= 0) {
                newCriteria = toPassCriteria(pass);
            }
        }

        std::lock_guard lock(mutex);
        if (newCriteria) {
            criteria = newCriteria;
        }
        switch (frame.status) {
        case ST_FRAME_STATUS_COMPLETE:
            ++interval.frames.complete;
            break;
        case ST_FRAME_STATUS_RECONSTRUCTED:
            ++interval.frames.reconstructed;
            break;
        case ST_FRAME_STATUS_CORRUPTED:
            ++interval.frames.corrupted;
            break;
        default:
            ++interval.frames.dropped;
            break;
        }
        if (frame.tp[MTL_SESSION_PORT_P] != nullptr) {
            interval.primary.add(toFrameTiming(*frame.tp[MTL_SESSION_PORT_P]));
        }
        if (interval.redundant && frame.tp[MTL_SESSION_PORT_R] != nullptr) {
            interval.redundant->add(toFrameTiming(*frame.tp[MTL_SESSION_PORT_R]));
        }
    }

    bool hasCriteria() const {
        std::lock_guard lock(mutex);
        return criteria.has_value();
    }
};

std::unique_ptr<St20TimingReceiver> St20TimingReceiver::create(const Options& options, std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    if (options.redundant) {
        impl->interval.redundant = TimingStats{};
    }

    struct mtl_init_params params {};
    const PathOptions* paths[2] = {&options.primary, options.redundant ? &*options.redundant : nullptr};
    params.num_ports = options.redundant ? 2 : 1;
    for (int i = 0; i < params.num_ports; ++i) {
        const PathOptions& path = *paths[i];
        std::snprintf(params.port[i], MTL_PORT_MAX_LEN, "%s", path.port.c_str());
        params.pmd[i] = mtl_pmd_by_port_name(params.port[i]);
        if (!isKernelPort(path.port) && !toIpv4(path.localIp, params.sip_addr[i])) {
            error = "Ungültige eigene Adresse: " + path.localIp;
            return nullptr;
        }
        params.rx_queues_cnt[i] = 1; // eine Empfangssitzung
        params.tx_queues_cnt[i] = 0; // nur Empfang
    }
    params.flags |= MTL_FLAG_BIND_NUMA | MTL_FLAG_DEV_AUTO_START_STOP;
    if (options.hwTimestamp) {
        params.flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;
    }
    if (options.ptp) {
        params.flags |= MTL_FLAG_PTP_ENABLE;
    }
    params.log_level = toMtlLogLevel(options.logLevel);
    if (!options.lcores.empty()) {
        impl->lcores = options.lcores;
        params.lcores = impl->lcores.data();
    }

    impl->mtl = mtl_init(&params);
    if (impl->mtl == nullptr) {
        error = "mtl_init fehlgeschlagen (Port an DPDK gebunden? Hugepages? Rechte? Details im MTL-Log).";
        return nullptr;
    }

    struct st20p_rx_ops ops {};
    ops.name = "st20-timing";
    ops.priv = impl.get();
    ops.port.num_port = params.num_ports;
    for (int i = 0; i < params.num_ports; ++i) {
        const PathOptions& path = *paths[i];
        std::snprintf(ops.port.port[i], MTL_PORT_MAX_LEN, "%s", path.port.c_str());
        if (!toIpv4(path.streamIp, ops.port.ip_addr[i])) {
            error = "Ungültige Adresse des Stroms: " + path.streamIp;
            return nullptr;
        }
        if (!path.sourceIp.empty() && !toIpv4(path.sourceIp, ops.port.mcast_sip_addr[i])) {
            error = "Ungültige Quelladresse: " + path.sourceIp;
            return nullptr;
        }
        ops.port.udp_port[i] = path.udpPort;
    }
    ops.port.payload_type = options.payloadType;
    ops.width = options.width;
    ops.height = options.height;
    ops.fps = toMtlFps(options.rate);
    ops.interlaced = options.interlaced;
    ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
    // Ausgabe im Transportformat: keine Umwandlung, die Bilder werden nur gezählt und verworfen.
    ops.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    ops.device = ST_PLUGIN_DEVICE_AUTO;
    ops.framebuff_cnt = 3;
    ops.flags = ST20P_RX_FLAG_BLOCK_GET | ST20P_RX_FLAG_TIMING_PARSER_META;

    impl->session = st20p_rx_create(impl->mtl, &ops);
    if (impl->session == nullptr) {
        error = "st20p_rx_create fehlgeschlagen (Format, Adresse oder Queues prüfen; Details im MTL-Log).";
        return nullptr;
    }

    Impl* raw = impl.get();
    impl->worker = std::thread([raw] { raw->run(); });
    return std::unique_ptr<St20TimingReceiver>(new St20TimingReceiver(std::move(impl)));
}

St20TimingReceiver::St20TimingReceiver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

St20TimingReceiver::~St20TimingReceiver() = default;

St20TimingReceiver::Interval St20TimingReceiver::takeInterval() {
    std::lock_guard lock(impl_->mutex);
    Interval result = impl_->interval;
    impl_->interval.frames = FrameCounters{};
    impl_->interval.primary.reset();
    if (impl_->interval.redundant) {
        impl_->interval.redundant->reset();
    }
    return result;
}

std::optional<PassCriteria> St20TimingReceiver::passCriteria() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->criteria;
}

void St20TimingReceiver::stop() {
    impl_->stopWorker();
}

} // namespace st2110
