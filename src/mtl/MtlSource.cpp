#include "mtl/MtlSource.h"

#include "core/TestSender.h"
#include "core/TestSignal.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mtl_api.h>
#include <mutex>
#include <st20_api.h>
#include <st30_api.h>
#include <st30_pipeline_api.h>
#include <st_pipeline_api.h>
#include <thread>
#include <vector>

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

// Eine Sitzung des Testsenders: eigener Thread, der Bilder bzw. Tonblöcke füllt. MTL taktet die
// Ausgabe (get_frame blockiert, bis ein Puffer frei ist) und setzt die Zeitstempel aus der PTP-Zeit.
struct TxSession : TxBackend::Session {
    FlowConfig flow;
    st20p_tx_handle video = nullptr;
    st30p_tx_handle audio = nullptr;
    std::unique_ptr<TestPatternGenerator> pattern;
    std::atomic<bool> stopping{false};
    std::thread thread;

    ~TxSession() override { stop(); }

    void stop() {
        stopping = true;
        if (thread.joinable()) {
            thread.join();
        }
        if (video != nullptr) {
            st20p_tx_free(video);
            video = nullptr;
        }
        if (audio != nullptr) {
            st30p_tx_free(audio);
            audio = nullptr;
        }
    }

    // Port und Ziel eintragen (ohne ST 2022-7).
    static bool fillPort(struct st_tx_port& port, const std::string& txPort, const FlowConfig& flow,
                         std::string& error) {
        port.num_port = 1;
        std::snprintf(port.port[0], MTL_PORT_MAX_LEN, "%s", txPort.c_str());
        if (!toIpv4(flow.primary().ip, port.dip_addr[0])) {
            error = "Ungültige Zieladresse: " + flow.primary().ip;
            return false;
        }
        port.udp_port[0] = flow.primary().udpPort;
        port.payload_type = flow.payloadType();
        return true;
    }

    bool open(mtl_handle mtl, const std::string& txPort, std::string& error) {
        if (flow.video) {
            const auto& v = *flow.video;
            struct st20p_tx_ops ops {};
            ops.name = flow.label.c_str();
            ops.priv = this;
            if (!fillPort(ops.port, txPort, flow, error)) {
                return false;
            }
            ops.width = v.format.width;
            ops.height = v.format.height;
            ops.fps = toMtlFps(v.format.rate);
            ops.interlaced = v.format.interlaced;
            // Das Testbild liegt schon im Transportformat vor: keine Wandlung in MTL.
            ops.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
            ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
            ops.transport_pacing = ST21_PACING_NARROW;
            ops.device = ST_PLUGIN_DEVICE_AUTO;
            ops.framebuff_cnt = 3;
            ops.flags = ST20P_TX_FLAG_BLOCK_GET;
            pattern =
                std::make_unique<TestPatternGenerator>(v.format.width, v.format.height, v.format.interlaced);
            video = st20p_tx_create(mtl, &ops);
            if (video == nullptr) {
                error = "st20p_tx_create fehlgeschlagen (Format, Adresse, Sendeport; Details im MTL-Log).";
                return false;
            }
            thread = std::thread([this] { videoLoop(); });
            return true;
        }
        const auto& a = *flow.audio;
        struct st30p_tx_ops ops {};
        ops.name = flow.label.c_str();
        ops.priv = this;
        if (!fillPort(ops.port, txPort, flow, error)) {
            return false;
        }
        ops.fmt = a.format.bitDepth == 16 ? ST30_FMT_PCM16 : ST30_FMT_PCM24;
        ops.channel = a.format.channels;
        ops.sampling = a.format.sampleRate == 96000 ? ST30_SAMPLING_96K : ST30_SAMPLING_48K;
        ops.ptime = toMtlPtime(a.format.ptime);
        ops.framebuff_cnt = 4;
        // Blöcke zu 10 ms: wenig Aufwand je Block, Ton bleibt nah an der Echtzeit.
        const int size = st30_calculate_framebuff_size(ops.fmt, ops.ptime, ops.sampling, ops.channel,
                                                       10ULL * 1000 * 1000, nullptr);
        if (size <= 0) {
            error = "Tonformat wird von MTL nicht unterstützt (st30_calculate_framebuff_size).";
            return false;
        }
        ops.framebuff_size = static_cast<uint32_t>(size);
        ops.flags = ST30P_TX_FLAG_BLOCK_GET;
        audio = st30p_tx_create(mtl, &ops);
        if (audio == nullptr) {
            error = "st30p_tx_create fehlgeschlagen (Format, Adresse, Sendeport; Details im MTL-Log).";
            return false;
        }
        thread = std::thread([this] { audioLoop(); });
        return true;
    }

    void videoLoop() {
        const auto& format = flow.video->format;
        // Bilder bzw. Halbbilder je Sekunde (bei Zeilensprung nennt format.rate wie bei MTL die Halbbilder):
        // der Inhalt läuft gleichmäßig, unabhängig von der Uhrzeit.
        const double rate = 1e9 / framePeriodNs(format.rate);
        uint64_t index = 0;
        while (!stopping) {
            struct st_frame* frame = st20p_tx_get_frame(video);
            if (frame == nullptr) {
                continue; // Zeitüberschreitung (1 s), z. B. solange der Link fehlt
            }
            const bool secondField = format.interlaced && (index % 2 == 1);
            frame->second_field = secondField;
            pattern->render(static_cast<uint8_t*>(frame->addr[0]), frame->linesize[0],
                            static_cast<double>(index) / rate, secondField);
            st20p_tx_put_frame(video, frame);
            ++index;
        }
    }

    void audioLoop() {
        const auto& format = flow.audio->format;
        const size_t bytesPerFrame = static_cast<size_t>(format.channels) * (format.bitDepth / 8u);
        uint64_t sampleIndex = 0;
        while (!stopping) {
            struct st30_frame* frame = st30p_tx_get_frame(audio);
            if (frame == nullptr) {
                continue;
            }
            const size_t frames = frame->buffer_size / bytesPerFrame;
            renderTestTonePcm(static_cast<uint8_t*>(frame->addr), sampleIndex, frames, format.channels,
                              format.sampleRate, format.bitDepth);
            frame->data_size = frames * bytesPerFrame;
            st30p_tx_put_frame(audio, frame);
            sampleIndex += frames;
        }
    }
};

struct FlowSession;

// Wandelt Vorschaubilder außerhalb des Datenpfads: der Rückruf von MTL reicht den Rahmen nur weiter,
// der Thread rechnet ihn um und gibt ihn danach an MTL zurück.
class PreviewWorker {
  public:
    struct Job {
        FlowSession* session;
        void* frame;
        size_t bytes;
        uint32_t width;
        uint32_t height;
    };
    PreviewWorker() : thread_([this] { run(); }) {}
    ~PreviewWorker() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void push(const Job& job) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(job);
        }
        cv_.notify_one();
    }

  private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    std::thread thread_;
};

// Eine MTL-Sitzung je Flow; priv der Rückrufe zeigt auf diese Struktur.
struct FlowSession {
    std::shared_ptr<FlowMonitor> monitor;
    st20_rx_handle video = nullptr;
    st30_rx_handle audio = nullptr;
    bool passKnown = false;
    PreviewWorker* previewWorker = nullptr;
    std::atomic<bool> previewBusy{false}; // ein Rahmen liegt beim PreviewWorker

    // --- Rückrufe aus dem Datenpfad von MTL (kurz halten) ---

    static int onVideoFrame(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
        auto* self = static_cast<FlowSession*>(priv);
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
        self->monitor->onVideoFrame(report);
        // Vorschau angefordert: Rahmen an den PreviewWorker, der ihn danach zurückgibt.
        auto& preview = self->monitor->videoPreview();
        if (self->previewWorker != nullptr && preview.due(self->monitor->clockNow()) &&
            meta->status != ST_FRAME_STATUS_CORRUPTED && !self->previewBusy.exchange(true)) {
            self->previewWorker->push({self, frame, meta->frame_total_size, meta->width, meta->height});
            return 0;
        }
        // Bildinhalt wird nicht gebraucht: sofort zurückgeben.
        st20_rx_put_framebuff(self->video, frame);
        return 0;
    }

    static int onAudioFrame(void* priv, void* frame, struct st30_rx_frame_meta* meta) {
        auto* self = static_cast<FlowSession*>(priv);
        auto& preview = self->monitor->audioPreview();
        if (preview.wanted(self->monitor->clockNow())) {
            preview.pushPcm(static_cast<const uint8_t*>(frame), meta->frame_recv_size,
                            self->monitor->config().audio->format.bitDepth);
        }
        st30_rx_put_framebuff(self->audio, frame);
        return 0;
    }

    static int onAudioTiming(void* priv, enum mtl_session_port port, struct st30_rx_tp_meta* tp) {
        auto* self = static_cast<FlowSession*>(priv);
        self->monitor->onAudioPeriod(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary,
                                     toAudioTiming(*tp));
        return 0;
    }

#ifdef ST20_RX_TP_PACKET_HOOK
    static void onVideoPacket(void* priv, enum mtl_session_port port, uint32_t rtp, uint64_t ns, int idx,
                              bool hw) {
        auto* self = static_cast<FlowSession*>(priv);
        self->monitor->onVideoPacket(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary, rtp, ns,
                                     idx, hw);
    }
#endif
#ifdef ST30_RX_TP_PACKET_HOOK
    static void onAudioPacket(void* priv, enum mtl_session_port port, uint32_t rtp, uint64_t ns, bool hw) {
        auto* self = static_cast<FlowSession*>(priv);
        self->monitor->onAudioPacket(port == MTL_SESSION_PORT_R ? Path::Redundant : Path::Primary, rtp, ns,
                                     hw);
    }
#endif

    void free() {
        // Liegt noch ein Rahmen beim PreviewWorker, erst dessen Rückgabe abwarten.
        while (previewBusy.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (video != nullptr) {
            st20_rx_free(video);
            video = nullptr;
        }
        if (audio != nullptr) {
            st30_rx_free(audio);
            audio = nullptr;
        }
    }
};

void PreviewWorker::run() {
    while (true) {
        Job job{};
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;
            }
            job = jobs_.front();
            jobs_.pop_front();
        }
        FlowSession& s = *job.session;
        const auto& format = s.monitor->config().video->format;
        const size_t lineBytes = static_cast<size_t>(job.width) / 2 * 5;
        const auto lines =
            static_cast<uint32_t>(std::min<size_t>(lineBytes > 0 ? job.bytes / lineBytes : 0, job.height));
        const PreviewSettings settings = s.monitor->videoPreview().settings();
        PreviewImage image;
        const bool converted =
            convertRfc4175ToRgb(static_cast<const uint8_t*>(job.frame), job.bytes, job.width, lines,
                                format.interlaced, settings.maxWidth, image);
        // Rahmen sofort zurückgeben; die JPEG-Kodierung braucht ihn nicht mehr.
        st20_rx_put_framebuff(s.video, job.frame);
        s.previewBusy = false;
        if (converted && encodeJpeg(image, settings.quality)) {
            image.rgb = {};
            s.monitor->videoPreview().put(std::move(image), s.monitor->clockNow());
        }
    }
}

struct MtlSource::Impl {
    Options options;
    MonitorHub& hub;
    std::string lcores;
    mtl_handle mtl = nullptr;
    std::mutex mutex; // schützt sessions
    std::vector<std::unique_ptr<FlowSession>> sessions;
    std::atomic<bool> stopping{false};
    std::thread statsThread;
    std::unique_ptr<PreviewWorker> previewWorker = std::make_unique<PreviewWorker>();

    // Öffnet die Sendesitzungen des Testsenders (TestSender ruft open() aus einem Hintergrundthread: MTL
    // wartet beim Anlegen bis zu 3 min, bis PTP eingerastet ist).
    class TxOpener : public TxBackend {
      public:
        explicit TxOpener(Impl& impl) : impl_(impl) {}
        std::unique_ptr<Session> open(const FlowConfig& flow, std::string& error) override {
            if (impl_.stopping) {
                error = "Programm wird beendet.";
                return nullptr;
            }
            if (impl_.options.ptp) {
                impl_.hub.addEvent("info",
                                   "Testsender " + flow.label +
                                       " startet (MTL wartet, bis PTP eingerastet ist, höchstens 3 min).");
            }
            auto tx = std::make_unique<TxSession>();
            tx->flow = flow;
            if (!tx->open(impl_.mtl, impl_.options.txPort->port, error)) {
                impl_.hub.addEvent("error", "Testsender " + flow.label + ": " + error);
                return nullptr;
            }
            impl_.hub.addEvent("info", "Testsender " + flow.label + " sendet " + flow.formatName() + " an " +
                                           flow.primary().ip + ":" + std::to_string(flow.primary().udpPort));
            return tx;
        }

      private:
        Impl& impl_;
    };
    std::unique_ptr<TxOpener> txOpener;
    std::unique_ptr<TestSender> testSender;

    Impl(const Options& o, MonitorHub& h) : options(o), hub(h) {}

    ~Impl() {
        stopping = true;
        if (statsThread.joinable()) {
            statsThread.join();
        }
        testSender.reset();    // Sendethreads beenden, Sitzungen freigeben (wartet auf laufende Starts)
        previewWorker.reset(); // offene Rahmen werden noch zurückgegeben
        for (auto& s : sessions) {
            s->free();
        }
        sessions.clear();
        if (mtl != nullptr) {
            mtl_uninit(mtl);
        }
    }

    static void onPtpSync(void* priv, struct mtl_ptp_sync_notify_meta* meta) {
        auto* self = static_cast<Impl*>(priv);
        self->hub.onPtpSync(meta->delta, meta->master_utc_offset);
    }

    // --- Statistik (eigener Thread) ---

    void statsLoop() {
        while (!stopping) {
            for (int i = 0; i < 10 && !stopping; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::lock_guard lock(mutex);
            for (auto& s : sessions) {
                if (s->video != nullptr) {
                    struct st20_rx_user_stats stats {};
                    if (st20_rx_get_session_stats(s->video, &stats) >= 0) {
                        s->monitor->setCounters(toCounters(stats.common, stats.stat_frames_incomplete));
                    }
                    if (!s->passKnown) {
                        struct st20_rx_tp_pass pass {};
                        if (st20_rx_timing_parser_critical(s->video, &pass) >= 0) {
                            s->monitor->setVideoPassCriteria(toPassCriteria(pass));
                            s->passKnown = true;
                        }
                    }
                }
                if (s->audio != nullptr) {
                    struct st30_rx_user_stats stats {};
                    if (st30_rx_get_session_stats(s->audio, &stats) >= 0) {
                        s->monitor->setCounters(toCounters(stats.common, stats.stat_frames_incomplete));
                    }
                }
            }
        }
    }

    // Port-Namen und Adressen einer Sitzung; liefert false bei ungültigen Adressen.
    template <typename Ops>
    bool fillPorts(Ops& ops, const FlowAddress& primary, const std::optional<FlowAddress>& redundant,
                   std::string& error) {
        if (redundant && !options.redundantPort) {
            error = "Redundanter Flow ohne Port R (--red-port).";
            return false;
        }
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

    // Startet die MTL-Sitzung für einen Flow des Hubs.
    bool openSession(FlowSession& session, std::string& error) {
        const FlowConfig& c = session.monitor->config();
        if (c.video) {
            const auto& v = *c.video;
            struct st20_rx_ops ops {};
            ops.name = c.id.c_str();
            ops.priv = &session;
            if (!fillPorts(ops, v.primary, v.redundant, error)) {
                return false;
            }
            ops.type = ST20_TYPE_FRAME_LEVEL;
            ops.width = v.format.width;
            ops.height = v.format.height;
            ops.fps = toMtlFps(v.format.rate);
            ops.interlaced = v.format.interlaced;
            ops.fmt = ST20_FMT_YUV_422_10BIT;
            ops.payload_type = v.payloadType;
            ops.framebuff_cnt = 4; // einer kann kurz beim PreviewWorker liegen
            ops.notify_frame_ready = &FlowSession::onVideoFrame;
            ops.flags = ST20_RX_FLAG_TIMING_PARSER_META | ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
#ifdef ST20_RX_TP_PACKET_HOOK
            ops.notify_tp_packet = &FlowSession::onVideoPacket;
#endif
            session.video = st20_rx_create(mtl, &ops);
            if (session.video == nullptr) {
                error =
                    "st20_rx_create fehlgeschlagen (Format, Adresse oder Queues prüfen; Details im MTL-Log).";
                return false;
            }
            return true;
        }
        const auto& a = *c.audio;
        struct st30_rx_ops ops {};
        ops.name = c.id.c_str();
        ops.priv = &session;
        if (!fillPorts(ops, a.primary, a.redundant, error)) {
            return false;
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
            return false;
        }
        ops.framebuff_size = static_cast<uint32_t>(size);
        ops.framebuff_cnt = 4;
        ops.notify_frame_ready = &FlowSession::onAudioFrame;
        ops.notify_timing_parser_result = &FlowSession::onAudioTiming;
        ops.flags = ST30_RX_FLAG_TIMING_PARSER_META;
#ifdef ST30_RX_TP_PACKET_HOOK
        ops.notify_tp_packet = &FlowSession::onAudioPacket;
#endif
        session.audio = st30_rx_create(mtl, &ops);
        if (session.audio == nullptr) {
            error = "st30_rx_create fehlgeschlagen (Format, Adresse oder Queues prüfen; Details im MTL-Log).";
            return false;
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

std::unique_ptr<MtlSource> MtlSource::create(const Options& options, MonitorHub& hub, std::string& error) {
    auto impl = std::make_unique<Impl>(options, hub);

    struct mtl_init_params params {};
    // Ports: P, dann R (ST 2022-7), dann der Sendeport des Testsenders.
    std::vector<const PortOptions*> ports{&options.primaryPort};
    if (options.redundantPort) {
        ports.push_back(&*options.redundantPort);
    }
    const int txIndex = options.txPort ? static_cast<int>(ports.size()) : -1;
    if (options.txPort) {
        ports.push_back(&*options.txPort);
    }
    params.num_ports = static_cast<uint8_t>(ports.size());
    for (int i = 0; i < params.num_ports; ++i) {
        std::snprintf(params.port[i], MTL_PORT_MAX_LEN, "%s", ports[i]->port.c_str());
        params.pmd[i] = mtl_pmd_by_port_name(params.port[i]);
        if (!isKernelPort(ports[i]->port) && !toIpv4(ports[i]->localIp, params.sip_addr[i])) {
            error = "Ungültige eigene Adresse: " + ports[i]->localIp;
            return nullptr;
        }
        // Je Flow eine Empfangs-Queue; auch für Flows, die erst zur Laufzeit dazukommen.
        params.rx_queues_cnt[i] = static_cast<uint16_t>(options.maxFlows);
        params.tx_queues_cnt[i] = 0; // nur Empfang
    }
    if (txIndex >= 0) {
        // Eine Queue je Videosender, alle Tonsender teilen sich eine; reserviert für die Höchstzahl, weil
        // Sender auch zur Laufzeit dazukommen.
        params.rx_queues_cnt[txIndex] = 0; // PTP und ARP laufen über die Systemqueue
        params.tx_queues_cnt[txIndex] =
            st_tx_sessions_queue_cnt(static_cast<uint16_t>(options.maxTxFlows), 1, 0, 0);
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
    if (!hasPacketHook()) {
        hub.addEvent("warn", "MTL ohne Paket-Rückruf gebaut: Histogramme und Paketprofile fehlen "
                             "(patches/mtl anwenden, siehe README).");
    }
    std::unique_ptr<MtlSource> source(new MtlSource(std::move(impl)));
    for (const auto& flow : options.flows) {
        std::string flowError;
        if (source->addFlow(flow, flowError).empty()) {
            error = (flow.label.empty() ? flow.primary().ip : flow.label) + ": " + flowError;
            return nullptr;
        }
    }
    Impl* raw = source->impl_.get();
    source->impl_->statsThread = std::thread([raw] { raw->statsLoop(); });
    if (options.txPort) {
        raw->txOpener = std::make_unique<Impl::TxOpener>(*raw);
        raw->testSender = std::make_unique<TestSender>(*raw->txOpener, options.maxTxFlows,
                                                       options.txPort->port, options.txPort->localIp);
        // Die Sender aus der Kommandozeile starten im Hintergrund, damit Empfang und Web-Oberfläche
        // sofort laufen.
        for (const auto& flow : options.txFlows) {
            std::string id;
            std::string txError;
            if (!raw->testSender->add(flow, true, id, txError)) {
                hub.addEvent("error", "Testsender " + flow.label + ": " + txError);
            }
        }
    }
    return source;
}

TestSender* MtlSource::testSender() {
    return impl_->testSender.get();
}

std::string MtlSource::addFlow(const FlowConfig& flow, std::string& error) {
    auto monitor = impl_->hub.addFlow(flow, error);
    if (!monitor) {
        return {};
    }
    auto session = std::make_unique<FlowSession>();
    session->monitor = monitor;
    session->previewWorker = impl_->previewWorker.get();
    std::lock_guard lock(impl_->mutex);
    if (!impl_->openSession(*session, error)) {
        session->free();
        impl_->hub.removeFlow(monitor->id());
        return {};
    }
    impl_->sessions.push_back(std::move(session));
    return monitor->id();
}

bool MtlSource::removeFlow(const std::string& id, std::string& error) {
    {
        std::lock_guard lock(impl_->mutex);
        auto& list = impl_->sessions;
        auto it =
            std::find_if(list.begin(), list.end(), [&](const auto& s) { return s->monitor->id() == id; });
        if (it == list.end()) {
            error = "Flow " + id + " nicht gefunden.";
            return false;
        }
        // Erst die Sitzung beenden (keine Rückrufe mehr), dann den Flow entfernen.
        (*it)->free();
        list.erase(it);
    }
    impl_->hub.removeFlow(id);
    return true;
}

MtlSource::MtlSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MtlSource::~MtlSource() = default;

} // namespace st2110
