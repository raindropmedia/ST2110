#include "mtl/MtlSource.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mtl_api.h>
#include <st20_api.h>
#include <st30_api.h>
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

enum st30_ptime toMtlPtime(PacketTime p) {
    switch (p) {
    case PacketTime::P125us:
        return ST30_PTIME_125US;
    case PacketTime::P250us:
        return ST30_PTIME_250US;
    case PacketTime::P333us:
        return ST30_PTIME_333US;
    case PacketTime::P1ms:
        return ST30_PTIME_1MS;
    case PacketTime::P4ms:
        return ST30_PTIME_4MS;
    }
    return ST30_PTIME_1MS;
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

AudioTiming toAudioTiming(const struct st30_rx_tp_meta& tp) {
    AudioTiming out;
    out.dpvrMinUs = tp.dpvr_min;
    out.dpvrMaxUs = tp.dpvr_max;
    out.dpvrAvgUs = tp.dpvr_avg;
    out.iptMinNs = tp.ipt_min;
    out.iptMaxNs = tp.ipt_max;
    out.iptAvgNs = tp.ipt_avg;
    out.tsdfUs = tp.tsdf;
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

StreamCounters toCounters(const struct st_rx_user_stats& c, uint64_t framesIncomplete) {
    StreamCounters out;
    for (size_t i = 0; i < kPathCount; ++i) {
        out.paths[i].packets = c.port[i].packets;
        out.paths[i].bytes = c.port[i].bytes;
        out.paths[i].lost = c.port[i].lost_packets;
        out.paths[i].reordered = c.port[i].reordered_packets;
        out.paths[i].duplicates = c.port[i].duplicates_same_port;
        out.paths[i].errors = c.port[i].err_packets;
    }
    out.received = c.stat_pkts_received;
    out.lost = c.stat_lost_packets;
    out.unrecovered = c.stat_pkts_unrecovered;
    out.redundant = c.stat_pkts_redundant;
    out.wrongPayloadType = c.stat_pkts_wrong_pt_dropped;
    out.wrongSsrc = c.stat_pkts_wrong_ssrc_dropped;
    out.framesIncomplete = framesIncomplete;
    return out;
}

bool toIpv4(const std::string& text, uint8_t* out) {
    return inet_pton(AF_INET, text.c_str(), out) == 1;
}

} // namespace

struct MtlSource::Impl {
    Options options;
    Monitor& monitor;
    std::string lcores;
    mtl_handle mtl = nullptr;
    st20_rx_handle video = nullptr;
    st30_rx_handle audio = nullptr;
    std::atomic<bool> passKnown{false};
    std::atomic<bool> stopping{false};
    std::thread statsThread;

    Impl(const Options& o, Monitor& m) : options(o), monitor(m) {}

    ~Impl() {
        stopping = true;
        if (statsThread.joinable()) {
            statsThread.join();
        }
        if (video != nullptr) {
            st20_rx_free(video);
        }
        if (audio != nullptr) {
            st30_rx_free(audio);
        }
        if (mtl != nullptr) {
            mtl_uninit(mtl);
        }
    }

    // --- Rückrufe aus dem Datenpfad von MTL (kurz halten) ---

    static int onVideoFrame(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
        auto* self = static_cast<Impl*>(priv);
        VideoFrameReport report;
        switch (meta->status) {
        case ST_FRAME_STATUS_COMPLETE:
            report.status = FrameStatus::Complete;
            break;
        case ST_FRAME_STATUS_RECONSTRUCTED:
            report.status = FrameStatus::Reconstructed;
            break;
        case ST_FRAME_STATUS_CORRUPTED:
            report.status = FrameStatus::Corrupted;
            break;
        default:
            report.status = FrameStatus::Dropped;
            break;
        }
        report.rtpTimestamp = meta->rtp_timestamp;
        report.packetsTotal = meta->pkts_total;
        for (size_t i = 0; i < kPathCount; ++i) {
            report.packetsPerPath[i] = meta->pkts_recv[i];
            if (meta->tp[i] != nullptr && meta->tp[i]->pkts_cnt > 0) {
                report.timing[i] = toFrameTiming(*meta->tp[i]);
            }
        }
        // Bildinhalt wird nicht gebraucht: sofort zurückgeben.
        st20_rx_put_framebuff(self->video, frame);
        self->monitor.onVideoFrame(report);
        return 0;
    }

    static int onAudioFrame(void* priv, void* frame, struct st30_rx_frame_meta* /*meta*/) {
        auto* self = static_cast<Impl*>(priv);
        st30_rx_put_framebuff(self->audio, frame);
        return 0;
    }

    static int onAudioTiming(void* priv, enum mtl_session_port port, struct st30_rx_tp_meta* tp) {
        auto* self = static_cast<Impl*>(priv);
        self->monitor.onAudioPeriod(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary,
                                    toAudioTiming(*tp));
        return 0;
    }

    static void onPtpSync(void* priv, struct mtl_ptp_sync_notify_meta* meta) {
        auto* self = static_cast<Impl*>(priv);
        self->monitor.onPtpSync(meta->delta, meta->master_utc_offset);
    }

#ifdef ST20_RX_TP_PACKET_HOOK
    static void onVideoPacket(void* priv, enum mtl_session_port port, uint32_t rtp, uint64_t ns, int idx,
                              bool hw) {
        auto* self = static_cast<Impl*>(priv);
        self->monitor.onVideoPacket(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary, rtp, ns,
                                    idx, hw);
    }
#endif
#ifdef ST30_RX_TP_PACKET_HOOK
    static void onAudioPacket(void* priv, enum mtl_session_port port, uint32_t rtp, uint64_t ns, bool hw) {
        auto* self = static_cast<Impl*>(priv);
        self->monitor.onAudioPacket(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary, rtp, ns,
                                    hw);
    }
#endif

    // --- Statistik (eigener Thread) ---

    void statsLoop() {
        while (!stopping) {
            for (int i = 0; i < 10 && !stopping; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (video != nullptr) {
                struct st20_rx_user_stats s {};
                if (st20_rx_get_session_stats(video, &s) >= 0) {
                    monitor.setCounters(Media::Video, toCounters(s.common, s.stat_frames_incomplete));
                }
                if (!passKnown) {
                    struct st20_rx_tp_pass pass {};
                    if (st20_rx_timing_parser_critical(video, &pass) >= 0) {
                        monitor.setVideoPassCriteria(toPassCriteria(pass));
                        passKnown = true;
                    }
                }
            }
            if (audio != nullptr) {
                struct st30_rx_user_stats s {};
                if (st30_rx_get_session_stats(audio, &s) >= 0) {
                    monitor.setCounters(Media::Audio, toCounters(s.common, s.stat_frames_incomplete));
                }
            }
        }
    }

    // Port-Namen und Adressen einer Sitzung; liefert false bei ungültigen Adressen.
    template <typename Ops>
    bool fillPorts(Ops& ops, const FlowAddress& primary, const std::optional<FlowAddress>& redundant,
                   std::string& error) {
        const FlowAddress* flows[2] = {&primary, redundant ? &*redundant : nullptr};
        ops.num_port = redundant ? 2 : 1;
        for (int i = 0; i < ops.num_port; ++i) {
            const std::string& port = i == 0 ? options.primaryPort.port : options.redundantPort->port;
            std::snprintf(ops.port[i], MTL_PORT_MAX_LEN, "%s", port.c_str());
            if (!toIpv4(flows[i]->ip, ops.ip_addr[i])) {
                error = "Ungültige Adresse: " + flows[i]->ip;
                return false;
            }
            if (!flows[i]->sourceIp.empty() && !toIpv4(flows[i]->sourceIp, ops.mcast_sip_addr[i])) {
                error = "Ungültige Quelladresse: " + flows[i]->sourceIp;
                return false;
            }
            ops.udp_port[i] = flows[i]->udpPort;
        }
        return true;
    }
};

bool MtlSource::hasPacketHook() {
#if defined(ST20_RX_TP_PACKET_HOOK) && defined(ST30_RX_TP_PACKET_HOOK)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<MtlSource> MtlSource::create(const Options& options, Monitor& monitor, std::string& error) {
    auto impl = std::make_unique<Impl>(options, monitor);

    struct mtl_init_params params {};
    params.num_ports = options.redundantPort ? 2 : 1;
    const PortOptions* ports[2] = {&options.primaryPort,
                                   options.redundantPort ? &*options.redundantPort : nullptr};
    const uint16_t sessions = static_cast<uint16_t>((options.video ? 1 : 0) + (options.audio ? 1 : 0));
    for (int i = 0; i < params.num_ports; ++i) {
        std::snprintf(params.port[i], MTL_PORT_MAX_LEN, "%s", ports[i]->port.c_str());
        params.pmd[i] = mtl_pmd_by_port_name(params.port[i]);
        if (!isKernelPort(ports[i]->port) && !toIpv4(ports[i]->localIp, params.sip_addr[i])) {
            error = "Ungültige eigene Adresse: " + ports[i]->localIp;
            return nullptr;
        }
        params.rx_queues_cnt[i] = sessions;
        params.tx_queues_cnt[i] = 0; // nur Empfang
    }
    params.flags |= MTL_FLAG_BIND_NUMA | MTL_FLAG_DEV_AUTO_START_STOP;
    if (options.hwTimestamp) {
        params.flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;
    }
    if (options.ptp) {
        params.flags |= MTL_FLAG_PTP_ENABLE;
        params.ptp_sync_notify = &Impl::onPtpSync;
    }
    params.priv = impl.get();
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

    if (options.video) {
        const auto& v = *options.video;
        struct st20_rx_ops ops {};
        ops.name = "st2110-video";
        ops.priv = impl.get();
        if (!impl->fillPorts(ops, v.primary, v.redundant, error)) {
            return nullptr;
        }
        ops.type = ST20_TYPE_FRAME_LEVEL;
        ops.width = v.format.width;
        ops.height = v.format.height;
        ops.fps = toMtlFps(v.format.rate);
        ops.interlaced = v.format.interlaced;
        ops.fmt = ST20_FMT_YUV_422_10BIT;
        ops.payload_type = v.payloadType;
        ops.framebuff_cnt = 3;
        ops.notify_frame_ready = &Impl::onVideoFrame;
        ops.flags = ST20_RX_FLAG_TIMING_PARSER_META | ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
#ifdef ST20_RX_TP_PACKET_HOOK
        ops.notify_tp_packet = &Impl::onVideoPacket;
#endif
        impl->video = st20_rx_create(impl->mtl, &ops);
        if (impl->video == nullptr) {
            error = "st20_rx_create fehlgeschlagen (Format, Adresse oder Queues prüfen; Details im MTL-Log).";
            return nullptr;
        }
    }

    if (options.audio) {
        const auto& a = *options.audio;
        struct st30_rx_ops ops {};
        ops.name = "st2110-audio";
        ops.priv = impl.get();
        if (!impl->fillPorts(ops, a.primary, a.redundant, error)) {
            return nullptr;
        }
        ops.type = ST30_TYPE_FRAME_LEVEL;
        ops.fmt = a.format.bitDepth == 16 ? ST30_FMT_PCM16 : ST30_FMT_PCM24;
        ops.channel = a.format.channels;
        ops.sampling = a.format.sampleRate == 96000 ? ST30_SAMPLING_96K : ST30_SAMPLING_48K;
        ops.ptime = toMtlPtime(a.format.ptime);
        ops.payload_type = a.payloadType;
        double fps = 0;
        const int size = st30_calculate_framebuff_size(ops.fmt, ops.ptime, ops.sampling, ops.channel,
                                                       10ULL * 1000 * 1000, &fps);
        if (size <= 0) {
            error = "Tonformat wird von MTL nicht unterstützt (st30_calculate_framebuff_size).";
            return nullptr;
        }
        ops.framebuff_size = static_cast<uint32_t>(size);
        ops.framebuff_cnt = 4;
        ops.notify_frame_ready = &Impl::onAudioFrame;
        ops.notify_timing_parser_result = &Impl::onAudioTiming;
        ops.flags = ST30_RX_FLAG_TIMING_PARSER_META;
#ifdef ST30_RX_TP_PACKET_HOOK
        ops.notify_tp_packet = &Impl::onAudioPacket;
#endif
        impl->audio = st30_rx_create(impl->mtl, &ops);
        if (impl->audio == nullptr) {
            error = "st30_rx_create fehlgeschlagen (Format, Adresse oder Queues prüfen; Details im MTL-Log).";
            return nullptr;
        }
    }

    if (!hasPacketHook()) {
        monitor.addEvent("warn", "MTL ohne Paket-Rückruf gebaut: Histogramme und Paketprofile fehlen "
                                 "(patches/mtl anwenden, siehe README).");
    }
    Impl* raw = impl.get();
    impl->statsThread = std::thread([raw] { raw->statsLoop(); });
    return std::unique_ptr<MtlSource>(new MtlSource(std::move(impl)));
}

MtlSource::MtlSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MtlSource::~MtlSource() = default;

} // namespace st2110
