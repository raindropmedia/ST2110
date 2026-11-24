#include "demo/DemoSource.h"

#include "core/AncAnalyzer.h"
#include "core/HbrmtAnalyzer.h"
#include "core/Preview.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <random>
#include <sstream>

namespace st2110 {
namespace {

// TAI liegt 37 s vor UTC (Stand 2017, unverändert).
constexpr uint64_t kTaiOffsetNs = 37ULL * 1'000'000'000ULL;
constexpr uint32_t kSenderIp = (10u << 24) | (10u << 16) | 1; // 10.10.0.1

uint64_t taiNowNs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) +
           kTaiOffsetNs;
}

uint32_t ipOf(const std::string& text) {
    in_addr addr{};
    inet_pton(AF_INET, text.c_str(), &addr);
    return ntohl(addr.s_addr);
}

// Demo-ST-2022-6: SD-SDI 625 Zeilen, 25 Hz, 4:2:2 10 Bit (270 Mbit/s, rund 24 500 Datagramme je Sekunde).
constexpr uint8_t kDemoHbrmtFrame = 0x11;
constexpr uint8_t kDemoHbrmtRate = 0x18;
constexpr uint8_t kDemoHbrmtSample = 0x01;
constexpr double kDemoHbrmtPeriodNs = 40e6;

double epochNs(const FlowConfig& c) {
    if (c.video) {
        return framePeriodNs(c.video->format.rate);
    }
    if (c.anc) {
        return framePeriodNs(c.anc->format.rate);
    }
    if (c.hbrmt) {
        return kDemoHbrmtPeriodNs;
    }
    return packetTimeNs(c.audio->format.ptime);
}

uint32_t rtpTicks(uint64_t ns, uint64_t hz) {
    const uint64_t seconds = ns / 1'000'000'000ULL;
    const uint64_t rest = ns % 1'000'000'000ULL;
    return static_cast<uint32_t>(seconds * hz + rest * hz / 1'000'000'000ULL);
}

FlowConfig spec(Media media, const std::string& text) {
    FlowConfig f;
    std::string error;
    parseFlowSpec(media, text, f, error);
    return f;
}

const char* ptimeMs(PacketTime p) {
    switch (p) {
    case PacketTime::P125us:
        return "0.125";
    case PacketTime::P250us:
        return "0.25";
    case PacketTime::P333us:
        return "0.333";
    case PacketTime::P4ms:
        return "4";
    case PacketTime::P1ms:
        break;
    }
    return "1";
}

std::string exactRate(const VideoFormat& f) {
    // SDP nennt die Bildrate; bei Zeilensprung ist das die halbe Halbbildrate.
    FrameRate r = f.rate;
    if (f.interlaced) {
        r = f.rate == FrameRate::P50      ? FrameRate::P25
            : f.rate == FrameRate::P59_94 ? FrameRate::P29_97
                                          : FrameRate::P30;
    }
    switch (r) {
    case FrameRate::P59_94:
        return "60000/1001";
    case FrameRate::P29_97:
        return "30000/1001";
    case FrameRate::P23_98:
        return "24000/1001";
    case FrameRate::P119_88:
        return "120000/1001";
    default:
        return frameRateName(r);
    }
}

// Einfache SDP nach ST 2110-20/-30 für die simulierten SAP-Ankündigungen.
std::string sdpFor(const std::string& name, const std::vector<FlowConfig>& flows) {
    std::ostringstream s;
    s << "v=0\r\no=- 1 1 IN IP4 10.10.0.1\r\ns=" << name << "\r\nt=0 0\r\n";
    int mid = 0;
    for (const auto& f : flows) {
        const int first = ++mid;
        const int second = f.redundant() ? ++mid : 0;
        if (second != 0) {
            s << "a=group:DUP m" << first << " m" << second << "\r\n";
        }
        for (int path = 0; path < (second != 0 ? 2 : 1); ++path) {
            const FlowAddress& a = path == 0 ? f.primary() : *f.redundant();
            const int pt = f.payloadType();
            s << "m=" << (f.video ? "video " : "audio ") << a.udpPort << " RTP/AVP " << pt << "\r\n";
            s << "c=IN IP4 " << a.ip << "/64\r\n";
            s << "a=source-filter: incl IN IP4 " << a.ip << " 10.10.0.1\r\n";
            if (f.video) {
                const auto& v = f.video->format;
                s << "a=rtpmap:" << pt << " raw/90000\r\n";
                s << "a=fmtp:" << pt << " sampling=YCbCr-4:2:2; width=" << v.width << "; height=" << v.height
                  << "; exactframerate=" << exactRate(v)
                  << "; depth=10; TCS=SDR; colorimetry=BT709; PM=2110GPM; "
                  << "SSN=ST2110-20:2017; TP=2110TPN" << (v.interlaced ? "; interlace" : "") << "\r\n";
            } else {
                const auto& af = f.audio->format;
                s << "a=rtpmap:" << pt << " L" << static_cast<int>(af.bitDepth) << "/" << af.sampleRate << "/"
                  << af.channels << "\r\n";
                s << "a=ptime:" << ptimeMs(af.ptime) << "\r\n";
            }
            s << "a=mid:m" << (path == 0 ? first : second) << "\r\n";
        }
    }
    return s.str();
}

} // namespace

const std::vector<DemoSource::Preset>& DemoSource::presets() {
    static const std::vector<Preset> kPresets{
        {"ideal", "Ideal", "Sender nach ST 2110-21 narrow, Ton ohne Jitter, PTP eingerastet"},
        {"jitter", "Jitter", "Paketabstände mit 1,5 µs Streuung (IPT-Histogramm wird breit)"},
        {"burst", "Bündel", "Je 8 Pakete auf einmal: Cinst steigt, Urteil wide"},
        {"late", "Zu spät", "Erstes Paket 40 µs nach TR_OFFSET: FPT-Fehler, Urteil failed"},
        {"loss", "Paketverlust", "Jedes 2000. Paket fehlt auf beiden Pfaden"},
        {"skew", "Pfadversatz", "Pfad R 150 µs später als P (ST 2022-7)"},
        {"audio", "Ton-Jitter", "Tonpakete mit 600 µs Streuung: TSDF steigt, Urteil wide"},
        {"ptp", "PTP unruhig", "Korrekturen von mehreren µs: PTP nicht eingerastet"},
    };
    return kPresets;
}

std::unique_ptr<DemoSource::Generator> DemoSource::makeGenerator(const FlowConfig& config,
                                                                 std::shared_ptr<FlowMonitor> monitor) {
    auto g = std::make_unique<Generator>();
    g->config = config;
    g->monitor = std::move(monitor);
    const VideoFormat vf = config.video ? config.video->format : VideoFormat{};
    const AudioFormat af = config.audio ? config.audio->format : AudioFormat{};
    g->senderP = std::make_unique<SyntheticSender>(vf, af, seed_++);
    g->senderR = std::make_unique<SyntheticSender>(vf, af, seed_++);
    if (g->monitor && config.video) {
        g->monitor->setVideoPacketsPerFrame(g->senderP->videoParams().packetsPerFrame);
    }
    return g;
}

DemoSource::DemoSource(const Options& options, MonitorHub& hub) : hub_(hub) {
    // Weitere Flows im simulierten Netz (nur über die Erkennung sichtbar, per Klick überwachbar).
    background_ = {
        spec(Media::Video, "239.100.0.21:20000,red=239.101.0.21:20000,format=1280x720p50,name=Kamera 3"),
        spec(Media::Audio, "239.100.0.22:30000,format=L24/48000/2,ptime=1,name=Kamera 3 Ton"),
        spec(Media::Audio, "239.100.0.31:30000,format=L24/48000/16,ptime=0.125,name=Saal-Mikrofone"),
        spec(Media::Anc, "239.100.0.23:40000,format=1920x1080p50,pt=100,name=Zusatzdaten Kamera 1"),
        spec(Media::Hbrmt, "239.100.0.40:5004,pt=98,name=SDI-Zuspieler (ST 2022-6)"),
        spec(Media::Video,
             "239.100.0.51:20000,format=1920x1080p25,sampling=RGB,depth=12,packing=GPM,name=Grafik RGB"),
        spec(Media::Video,
             "239.100.0.52:20000,format=1280x720p50,sampling=YCbCr-4:2:0,depth=8,name=Zuspieler 4:2:0"),
        spec(Media::Video,
             "239.100.0.53:20000,format=1920x1080i50,sampling=YCbCr-4:2:2,depth=8,name=Kamera 8 Bit"),
    };
    for (const auto& flow : options.flows) {
        std::string error;
        addFlow(flow, error);
        if (!error.empty()) {
            hub_.addEvent("error", "Demo: " + error);
        }
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& b : background_) {
            generators_.push_back(makeGenerator(b, nullptr));
        }
        updatePreviewTargets();
    }
    hub_.setDiscoveryStatus(nlohmann::json{
        {"interface", "demo"}, {"active", true}, {"sap", true}, {"error", nullptr}, {"sapError", nullptr}});
    hub_.addEvent("info", "Demo gestartet: synthetische Flows ohne Karte");
    thread_ = std::thread([this] { run(); });
    previewThread_ = std::thread([this] { previewLoop(); });
}

DemoSource::~DemoSource() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (previewThread_.joinable()) {
        previewThread_.join();
    }
}

std::string DemoSource::addFlow(const FlowConfig& flow, std::string& error) {
    auto monitor = hub_.addFlow(flow, error);
    if (!monitor) {
        return {};
    }
    std::lock_guard lock(mutex_);
    // Läuft der Flow schon im simulierten Netz, nur den Monitor anhängen.
    for (auto& g : generators_) {
        if (!g->monitor && g->config.primary().ip == flow.primary().ip &&
            g->config.primary().udpPort == flow.primary().udpPort) {
            g->monitor = monitor;
            if (flow.video) {
                monitor->setVideoPacketsPerFrame(g->senderP->videoParams().packetsPerFrame);
            }
            updatePreviewTargets();
            return monitor->id();
        }
    }
    generators_.push_back(makeGenerator(monitor->config(), monitor));
    updatePreviewTargets();
    return monitor->id();
}

// Aufruf unter mutex_: Videoflows mit Monitor für den Vorschau-Thread übernehmen.
void DemoSource::updatePreviewTargets() {
    std::vector<PreviewTarget> targets;
    for (size_t i = 0; i < generators_.size(); ++i) {
        const auto& g = *generators_[i];
        if (g.monitor && g.config.video) {
            targets.push_back({g.monitor, g.config.video->format, static_cast<int>(i)});
        }
    }
    std::lock_guard lock(previewMutex_);
    previewTargets_ = std::move(targets);
}

bool DemoSource::removeFlow(const std::string& id, std::string& error) {
    {
        std::lock_guard lock(mutex_);
        for (auto it = generators_.begin(); it != generators_.end(); ++it) {
            if ((*it)->monitor && (*it)->monitor->id() == id) {
                const bool background =
                    std::any_of(background_.begin(), background_.end(), [&](const FlowConfig& b) {
                        return b.primary().ip == (*it)->config.primary().ip;
                    });
                if (background) {
                    (*it)->monitor.reset(); // bleibt im Netz sichtbar
                } else {
                    generators_.erase(it);
                }
                updatePreviewTargets();
                break;
            }
        }
    }
    if (!hub_.removeFlow(id)) {
        error = "Flow " + id + " nicht gefunden.";
        return false;
    }
    return true;
}

bool DemoSource::setPreset(const std::string& id) {
    SenderFaults f;
    double ptpNoise = 25;
    if (id == "jitter") {
        f.jitterNs = 1500;
    } else if (id == "burst") {
        f.burstPackets = 8;
    } else if (id == "late") {
        f.startOffsetNs = 40000;
    } else if (id == "loss") {
        f.lossRate = 0.0005;
    } else if (id == "skew") {
        f.pathSkewNs = 150000;
    } else if (id == "audio") {
        f.audioJitterUs = 600;
    } else if (id == "ptp") {
        ptpNoise = 4000;
    } else if (id != "ideal") {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        preset_ = id;
        faults_ = f;
        ptpNoiseNs_ = ptpNoise;
        glitch_ = f.lossRate > 0;
    }
    for (const auto& p : presets()) {
        if (p.id == id) {
            hub_.addEvent("info", "Demo: Störung „" + p.label + "“ – " + p.description);
        }
    }
    return true;
}

std::string DemoSource::preset() const {
    std::lock_guard lock(mutex_);
    return preset_;
}

void DemoSource::discoverVideo(const Generator& g, uint64_t frame, bool redundant, double /*skewNs*/) {
    const auto& v = g.config.video->format;
    const FlowAddress& a = redundant ? *g.config.redundant() : g.config.primary();
    const int packets = g.senderP->videoParams().packetsPerFrame;
    const uint32_t lines = v.interlaced ? v.height / 2 : v.height;
    const uint32_t rtp = static_cast<uint32_t>(static_cast<uint64_t>(
        static_cast<long double>(frame) * framePeriodNs(v.rate) * 90000.0L / 1e9L + 1e-6L));
    PacketInfo p;
    p.srcIp = kSenderIp;
    p.dstIp = ipOf(a.ip);
    p.srcPort = 10000;
    p.dstPort = a.udpPort;
    p.dscp = 34;
    p.wireBytes = 1262;
    p.rtp = true;
    p.payloadType = g.config.payloadType();
    p.ssrc = 0x1000 + p.dstIp;
    p.timestamp = rtp;
    p.rtpPayload = 1208;
    p.rfc4175 = true;
    p.videoBytes = 1200;
    p.secondField = v.interlaced && (frame % 2 == 1);
    // Wenige echte Pakete der ersten Zeile (für die Erkennung des Pixelformats), der Rest zusammengefasst,
    // dann das letzte Paket mit Marker am Ende der letzten Zeile.
    const PixelFormat pf = v.pixel;
    const uint32_t group = pf.pgroupBytes();
    const uint32_t bytesPerPacket = std::max<uint32_t>(1, 1200 / group) * group;
    const uint32_t pixelsPerPacket = bytesPerPacket / group * pf.pgroupPixels();
    p.rtpPayload = static_cast<uint16_t>(bytesPerPacket + 8);
    p.videoBytes = bytesPerPacket;
    p.segments = 1;
    p.firstLength = static_cast<uint16_t>(bytesPerPacket);
    p.lastLength = p.firstLength;
    p.sequence = static_cast<uint16_t>(frame * static_cast<uint64_t>(packets));
    p.row = 0;
    const int firstRow = std::min<int>(12, packets - 1);
    for (int k = 0; k < firstRow; ++k) {
        p.rowOffset = static_cast<uint16_t>(k * pixelsPerPacket);
        p.lastRow = p.row;
        p.lastOffset = p.rowOffset;
        hub_.onDiscoveredPacket(p, 1);
        ++p.sequence;
    }
    if (g.config.video->packing != Packing::Bpm) {
        p.segments = 2; // GPM: ein Paket trägt das Zeilenende und den Anfang der nächsten Zeile
        hub_.onDiscoveredPacket(p, 1);
        ++p.sequence;
        p.segments = 1;
    }
    if (packets - 1 > firstRow) {
        hub_.onDiscoveredPacket(p, static_cast<uint32_t>(packets - 1 - firstRow));
        p.sequence = static_cast<uint16_t>(p.sequence + packets - 1 - firstRow);
    }
    p.row = static_cast<uint16_t>(lines - 1);
    p.rowOffset = static_cast<uint16_t>(v.width - pixelsPerPacket);
    p.lastRow = p.row;
    p.lastOffset = p.rowOffset;
    p.marker = true;
    hub_.onDiscoveredPacket(p, 1);
}

void DemoSource::discoverAudio(Generator& g, uint64_t firstPacket, uint32_t count) {
    const auto& af = g.config.audio->format;
    for (int path = 0; path < (g.config.redundant() ? 2 : 1); ++path) {
        const FlowAddress& a = path == 0 ? g.config.primary() : *g.config.redundant();
        PacketInfo p;
        p.srcIp = kSenderIp;
        p.dstIp = ipOf(a.ip);
        p.srcPort = 10000;
        p.dstPort = a.udpPort;
        p.dscp = 46;
        p.rtpPayload = static_cast<uint16_t>(af.payloadBytes());
        p.wireBytes = p.rtpPayload + 54u;
        p.rtp = true;
        p.payloadType = g.config.payloadType();
        p.ssrc = 0x2000 + p.dstIp;
        p.sequence = static_cast<uint16_t>(firstPacket);
        p.timestamp = static_cast<uint32_t>(firstPacket * af.samplesPerPacket());
        hub_.onDiscoveredPacket(p, count);
    }
}

void DemoSource::emitVideo(Generator& g, uint64_t frame, const SenderFaults& faults) {
    const int packets = g.senderP->videoParams().packetsPerFrame;
    auto& c = g.counters;
    const int lostP = g.senderP->videoFrame(frame, faults, 0.0, [&](uint32_t rtp, uint64_t ns, int idx) {
        if (g.monitor) {
            g.monitor->onVideoPacket(Path::Primary, rtp, ns, idx, true);
        }
    });
    c.paths[0].packets += static_cast<uint64_t>(packets - lostP);
    c.paths[0].bytes += static_cast<uint64_t>(packets - lostP) * 1262;
    c.paths[0].lost += static_cast<uint64_t>(lostP);
    c.lost += static_cast<uint64_t>(lostP);
    c.received += static_cast<uint64_t>(packets);
    discoverVideo(g, frame, false, 0.0);
    if (g.config.redundant()) {
        const int lostR =
            g.senderR->videoFrame(frame, faults, faults.pathSkewNs, [&](uint32_t rtp, uint64_t ns, int idx) {
                if (g.monitor) {
                    g.monitor->onVideoPacket(Path::Redundant, rtp, ns, idx, true);
                }
            });
        c.paths[1].packets += static_cast<uint64_t>(packets - lostR);
        c.paths[1].bytes += static_cast<uint64_t>(packets - lostR) * 1262;
        c.paths[1].lost += static_cast<uint64_t>(lostR);
        c.lost += static_cast<uint64_t>(lostR);
        c.redundant += static_cast<uint64_t>(packets - lostR);
        discoverVideo(g, frame, true, faults.pathSkewNs);
    } else {
        c.unrecovered += static_cast<uint64_t>(lostP);
        c.received -= static_cast<uint64_t>(lostP);
    }
}

void DemoSource::emitAudio(Generator& g, uint64_t packet, const SenderFaults& faults) {
    auto& c = g.counters;
    const uint64_t bytes = g.config.audio->format.payloadBytes() + 54;
    if (g.senderP->audioPacket(packet, faults, 0.0, [&](uint32_t rtp, uint64_t ns) {
            if (g.monitor) {
                g.monitor->onAudioPacket(Path::Primary, rtp, ns, true);
            }
        })) {
        c.paths[0].packets++;
        c.paths[0].bytes += bytes;
        c.received++;
    } else {
        c.paths[0].lost++;
        c.lost++;
    }
    if (g.config.redundant()) {
        if (g.senderR->audioPacket(packet, faults, faults.pathSkewNs, [&](uint32_t rtp, uint64_t ns) {
                if (g.monitor) {
                    g.monitor->onAudioPacket(Path::Redundant, rtp, ns, true);
                }
            })) {
            c.paths[1].packets++;
            c.paths[1].bytes += bytes;
            c.redundant++;
        } else {
            c.paths[1].lost++;
            c.lost++;
        }
    }
}

void DemoSource::previewLoop() {
    // Testbilder in eigenem Thread: große Bilder und JPEG dürfen die Paketerzeugung nicht aufhalten.
    struct Job {
        std::shared_ptr<FlowMonitor> monitor;
        VideoFormat format;
        int index;
        bool glitch;
    };
    while (!stop_) {
        // Eigene, kurze Sperre: mutex_ hält der Paketgenerator je Takt lange.
        std::vector<Job> jobs;
        {
            std::lock_guard lock(previewMutex_);
            for (const auto& t : previewTargets_) {
                if (t.monitor->videoPreview().due(t.monitor->clockNow())) {
                    jobs.push_back({t.monitor, t.format, t.index, glitch_.load()});
                }
            }
        }
        for (const auto& job : jobs) {
            auto& preview = job.monitor->videoPreview();
            const PreviewSettings settings = preview.settings();
            const double now = job.monitor->clockNow();
            const uint32_t width = std::min(settings.maxWidth, job.format.width) & ~1u;
            const uint32_t height = std::max<uint32_t>(2, width * job.format.height / job.format.width);
            PreviewImage image;
            renderTestPattern(image, width, height, now, job.index, job.glitch);
            if (encodeJpeg(image, settings.quality)) {
                image.rgb = {};
                preview.put(std::move(image), now);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(jobs.empty() ? 2 : 1));
    }
}

void DemoSource::emitAnc(Generator& g, uint64_t frame, const SenderFaults& faults) {
    if (!g.monitor) {
        return;
    }
    const double period = framePeriodNs(g.config.anc->format.rate);
    const auto epoch = static_cast<uint64_t>(static_cast<long double>(frame) * period);
    AncFrameReport r;
    r.rtpTimestamp = rtpTicks(epoch, 90000);
    r.firstPacketNs = epoch + 300'000 + (g.packetCounter++ % 7) * 5'000; // 0,3 ms hinter der Sollzeit
    r.marker = true;
    auto add = [&](uint8_t did, uint8_t sdid, uint16_t line, uint16_t udw) {
        AncPacket p;
        p.did = did;
        p.sdid = sdid;
        p.line = line;
        p.horiOffset = 0;
        p.udwSize = udw;
        r.anc.push_back(p);
    };
    add(0x41, 0x01, 10, 4);  // Payload Identification
    add(0x60, 0x60, 9, 16);  // Zeitcode ATC
    add(0x41, 0x05, 11, 8);  // AFD
    add(0x61, 0x01, 12, 42); // CEA-708
    r.packets = 1;
    if (faults.lossRate > 0 && frame % 40 == 0) {
        r.seqLost = 1;
        r.seqDiscont = true;
        r.corrupted = true;
        r.anc.pop_back();
    }
    g.counters.paths[0].packets += r.packets;
    g.counters.paths[0].lost += r.seqLost;
    g.counters.received += r.packets;
    g.counters.lost += r.seqLost;
    g.monitor->onAncFrame(r);
}

void DemoSource::emitHbrmt(Generator& g, uint64_t frame, const SenderFaults& faults) {
    if (!g.monitor) {
        return;
    }
    const uint32_t dpf = hbrmtExpectedDatagrams(kDemoHbrmtFrame, kDemoHbrmtRate, kDemoHbrmtSample);
    const size_t headerBytes = 12 + 8 + 4;
    if (g.hbrmtBuffer.empty()) {
        g.hbrmtBuffer.assign(headerBytes + 1376, 0);
        g.hbrmtBuffer[0] = 0x80;
        g.hbrmtBuffer[8] = 0x53;
        g.hbrmtBuffer[9] = 0x44;
        g.hbrmtBuffer[10] = 0x49;
        g.hbrmtBuffer[11] = 0x01;
    }
    auto& b = g.hbrmtBuffer;
    const auto epoch = static_cast<uint64_t>(static_cast<long double>(frame) * kDemoHbrmtPeriodNs);
    const uint32_t baseTs = rtpTicks(epoch, 27'000'000);
    const auto step = static_cast<uint32_t>(kDemoHbrmtPeriodNs * 27e6 / 1e9 / dpf);
    // Nutzdaten-Header: F=1, FRCount, R=2 (UTC), CF=1 (27 MHz), FRAME/FRATE/SAMPLE
    const uint32_t w0 = (1u << 27) | (static_cast<uint32_t>(frame & 0xFF) << 16) | (2u << 14) | (1u << 5);
    const uint32_t w1 = (static_cast<uint32_t>(kDemoHbrmtFrame) << 20) |
                        (static_cast<uint32_t>(kDemoHbrmtRate) << 12) |
                        (static_cast<uint32_t>(kDemoHbrmtSample) << 8);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
        }
    };
    put32(12, w0);
    put32(16, w1);
    for (uint32_t i = 0; i < dpf; ++i) {
        const uint32_t ts = baseTs + i * step;
        const uint16_t seq = static_cast<uint16_t>(g.sequence++);
        b[1] = static_cast<uint8_t>((i + 1 == dpf ? 0x80 : 0x00) | 98);
        b[2] = static_cast<uint8_t>(seq >> 8);
        b[3] = static_cast<uint8_t>(seq);
        put32(4, ts);
        put32(20, ts); // Video-Zeitstempel im selben 27-MHz-Takt
        ++g.packetCounter;
        if (faults.lossRate > 0 && g.packetCounter % 2000 == 0) {
            continue; // Paket geht im Netz verloren
        }
        g.monitor->onHbrmtPacket(b.data(), b.size());
    }
    g.counters.paths[0].packets += dpf;
}

void DemoSource::previewAudio(Generator& g, uint64_t firstPacket, uint64_t packets) {
    if (!g.monitor || !g.monitor->audioPreview().wanted(g.monitor->clockNow())) {
        return;
    }
    const auto& af = g.config.audio->format;
    const uint64_t spp = af.samplesPerPacket();
    generateTestTone(g.toneBuffer, firstPacket * spp, static_cast<size_t>(packets * spp), af.channels,
                     af.sampleRate);
    g.monitor->audioPreview().pushFloat(g.toneBuffer.data(), static_cast<size_t>(packets * spp));
}

void DemoSource::announce() {
    // Kamera 3 (nur im Netz) und Kamera 1 (sofern vorhanden) wie ein Sender per SAP ankündigen.
    std::vector<FlowConfig> cam3(background_.begin(), background_.begin() + 2);
    SapMessage m;
    m.origin = "10.10.0.1";
    m.hash = 0x0301;
    m.sdp = sdpFor("Kamera 3", cam3);
    hub_.onSapMessage(m);
    std::vector<FlowConfig> mics{background_[2]};
    m.hash = 0x0401;
    m.sdp = sdpFor("Saal-Mikrofone", mics);
    hub_.onSapMessage(m);
}

void DemoSource::run() {
    std::mt19937_64 rng(99);
    auto nextPtp = std::chrono::steady_clock::now();
    auto nextCounters = nextPtp;
    auto nextSap = nextPtp;
    auto nextExtras = nextPtp;

    while (!stop_) {
        const uint64_t now = taiNowNs();
        {
            std::lock_guard lock(mutex_);
            const SenderFaults faults = faults_;
            for (auto& gp : generators_) {
                Generator& g = *gp;
                const double period = epochNs(g.config);
                const auto last = static_cast<uint64_t>(std::floor(static_cast<double>(now) / period));
                if (!g.started) {
                    g.next = last;
                    g.started = true;
                }
                // Nur Zeiträume erzeugen, die vollständig vergangen sind.
                if (g.config.video) {
                    for (; g.next + 1 < last && !stop_; ++g.next) {
                        emitVideo(g, g.next, faults);
                    }
                } else if (g.config.anc) {
                    for (; g.next + 1 < last && !stop_; ++g.next) {
                        emitAnc(g, g.next, faults);
                    }
                } else if (g.config.hbrmt) {
                    for (; g.next + 1 < last && !stop_; ++g.next) {
                        emitHbrmt(g, g.next, faults);
                    }
                } else {
                    const uint64_t first = g.next;
                    for (; g.next + 2 < last && !stop_; ++g.next) {
                        emitAudio(g, g.next, faults);
                    }
                    if (g.next > first) {
                        discoverAudio(g, first, static_cast<uint32_t>(g.next - first));
                        previewAudio(g, first, g.next - first);
                    }
                }
            }
        }
        const auto steady = std::chrono::steady_clock::now();
        if (steady >= nextPtp) {
            double noise;
            {
                std::lock_guard lock(mutex_);
                noise = ptpNoiseNs_;
            }
            std::normal_distribution<double> dist(0.0, noise);
            hub_.onPtpSync(static_cast<int64_t>(dist(rng)), 37);
            nextPtp += std::chrono::milliseconds(125);
        }
        if (steady >= nextExtras) {
            // PTP-Pakete und Zusatzdaten (ST 2110-40) im Netz
            PacketInfo ptp;
            ptp.srcIp = (10u << 24) | (10u << 16) | 254;
            ptp.dstIp = (224u << 24) | (0u << 16) | (1u << 8) | 129; // 224.0.1.129
            ptp.srcPort = 319;
            ptp.dstPort = 319;
            ptp.wireBytes = 86;
            hub_.onDiscoveredPacket(ptp, 2);
            ptp.dstPort = 320;
            hub_.onDiscoveredPacket(ptp, 2);
            PacketInfo anc;
            anc.srcIp = kSenderIp;
            anc.dstIp = (239u << 24) | (100u << 16) | 23;
            anc.srcPort = 10000;
            anc.dstPort = 40000;
            anc.wireBytes = 120;
            anc.rtp = true;
            anc.payloadType = 100;
            anc.rtpPayload = 66;
            anc.timestamp = static_cast<uint32_t>(static_cast<uint64_t>(now / 20'000'000ULL) * 1800ULL);
            hub_.onDiscoveredPacket(anc, 25);
            PacketInfo sdi;
            sdi.srcIp = kSenderIp;
            sdi.dstIp = (239u << 24) | (100u << 16) | 40;
            sdi.srcPort = 10000;
            sdi.dstPort = 5004;
            sdi.wireBytes = 1442;
            sdi.rtp = true;
            sdi.payloadType = 98;
            sdi.rtpPayload = 1388;
            sdi.timestamp = static_cast<uint32_t>(now * 27ULL / 1000ULL); // 27 MHz
            hub_.onDiscoveredPacket(sdi, 12000);
            nextExtras += std::chrono::milliseconds(500);
        }
        if (steady >= nextCounters) {
            std::lock_guard lock(mutex_);
            for (auto& g : generators_) {
                if (g->monitor) {
                    g->monitor->setCounters(g->counters);
                }
            }
            nextCounters += std::chrono::milliseconds(500);
        }
        if (steady >= nextSap) {
            announce();
            nextSap += std::chrono::seconds(30);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace st2110
