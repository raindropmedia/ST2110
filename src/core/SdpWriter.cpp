#include "core/SdpWriter.h"

#include <sstream>

namespace st2110 {

namespace {

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

// "exactframerate": bei Zeilensprung die Vollbildrate (halbe Halbbildrate)
std::string exactRate(const VideoFormat& f) {
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

std::string audioChannelOrder(uint16_t channels) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "SMPTE2110.(U%02u)", static_cast<unsigned>(channels));
    return buf;
}

} // namespace

std::string flowToSdp(const FlowConfig& f, const std::string& sourceIp) {
    std::ostringstream s;
    const std::string origin = sourceIp.empty() ? "0.0.0.0" : sourceIp;
    s << "v=0\r\no=- 1 1 IN IP4 " << origin << "\r\ns=" << (f.label.empty() ? "Flow" : f.label)
      << "\r\nt=0 0\r\n";
    const bool redundant = f.redundant().has_value();
    if (redundant) {
        s << "a=group:DUP primary secondary\r\n";
    }
    for (int path = 0; path < (redundant ? 2 : 1); ++path) {
        const FlowAddress& a = path == 0 ? f.primary() : *f.redundant();
        const int pt = f.payloadType();
        s << "m=" << (f.audio ? "audio " : "video ") << a.udpPort << " RTP/AVP " << pt << "\r\n";
        s << "c=IN IP4 " << a.ip << "/32\r\n";
        if (!sourceIp.empty()) {
            s << "a=source-filter: incl IN IP4 " << a.ip << " " << sourceIp << "\r\n";
        }
        if (f.video) {
            const auto& v = f.video->format;
            const bool rgb = v.pixel.sampling == Sampling::Rgb;
            s << "a=rtpmap:" << pt << " raw/90000\r\n";
            s << "a=fmtp:" << pt << " sampling=" << v.pixel.sdpSampling() << "; width=" << v.width
              << "; height=" << v.height << "; exactframerate=" << exactRate(v) << "; depth=" << v.pixel.depth
              << "; TCS=SDR; colorimetry=BT709; RANGE=" << (rgb ? "FULL" : "NARROW")
              << "; PM=" << (f.video->packing == Packing::Bpm ? "2110BPM" : "2110GPM")
              << "; SSN=ST2110-20:2017; TP=2110TPN" << (v.interlaced ? "; interlace" : "") << "\r\n";
        } else if (f.audio) {
            const auto& af = f.audio->format;
            s << "a=rtpmap:" << pt << " L" << static_cast<int>(af.bitDepth) << "/" << af.sampleRate << "/"
              << af.channels << "\r\n";
            s << "a=fmtp:" << pt << " channel-order=" << audioChannelOrder(af.channels) << "\r\n";
            s << "a=ptime:" << ptimeMs(af.ptime) << "\r\n";
        } else if (f.anc) {
            s << "a=rtpmap:" << pt << " smpte291/90000\r\n";
            // Der Testsender sendet Payload-ID, Zeitcode, AFD und CEA-708
            s << "a=fmtp:" << pt
              << " DID_SDID={0x41,0x01}; DID_SDID={0x60,0x60}; DID_SDID={0x41,0x05}; "
                 "DID_SDID={0x61,0x01}\r\n";
        } else {
            s << "a=rtpmap:" << pt << " SMPTE2022-6/27000000\r\n";
        }
        s << "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\n";
        if (redundant) {
            s << "a=mid:" << (path == 0 ? "primary" : "secondary") << "\r\n";
        }
    }
    return s.str();
}

} // namespace st2110
