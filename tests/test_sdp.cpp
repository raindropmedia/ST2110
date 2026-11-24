#include "core/Sdp.h"

#include <doctest/doctest.h>

using namespace st2110;

TEST_CASE("SDP: Sitzungsadresse, rtpmap, fmtp-Flags") {
    const std::string text =
        "v=0\r\n"
        "s=Test\r\n"
        "c=IN IP4 239.1.1.1/32\r\n"
        "m=video 5004 RTP/AVP 112\r\n"
        "a=rtpmap:112 raw/90000\r\n"
        "a=fmtp:112 sampling=YCbCr-4:2:2; width=1280; height=720; exactframerate=60000/1001; depth=10\r\n";
    std::string error;
    auto session = parseSdp(text, error);
    REQUIRE(session);
    CHECK(session->name == "Test");
    REQUIRE(session->media.size() == 1);
    CHECK(session->media[0].connectionIp == "239.1.1.1");
    CHECK(session->media[0].payloadType == 112);
    auto video = sdpVideo(*session, error);
    REQUIRE(video);
    CHECK(video->format.name() == "1280x720p59.94");
    CHECK(video->payloadType == 112);
    CHECK_FALSE(sdpAudio(*session, error));
}

TEST_CASE("SDP: nicht unterstützte Formate") {
    std::string error;
    auto session =
        parseSdp("v=0\nm=video 5004 RTP/AVP 96\nc=IN IP4 239.1.1.1\na=rtpmap:96 raw/90000\n"
                 "a=fmtp:96 sampling=CLYCbCr-4:2:2; width=1920; height=1080; exactframerate=50; depth=16f\n",
                 error);
    REQUIRE(session);
    CHECK_FALSE(sdpVideo(*session, error));
    CHECK(error.find("8, 10, 12 oder 16 Bit") != std::string::npos);
    CHECK_FALSE(parseSdp("kein sdp", error));
}

TEST_CASE("Videoformat und exakte Bildrate") {
    CHECK(parseVideoFormat("3840x2160p59.94")->rate == FrameRate::P59_94);
    CHECK_FALSE(parseVideoFormat("1920x1080x50"));
    CHECK(parseExactFrameRate("24000/1001") == FrameRate::P23_98);
    CHECK(parseExactFrameRate("25") == FrameRate::P25);
    CHECK_FALSE(parseExactFrameRate("48"));
}

TEST_CASE("SDP: alle Pixelformate von ST 2110-20 und Packmodus") {
    for (const auto& pf : allPixelFormats()) {
        std::string error;
        const std::string sdp = "v=0\nm=video 5004 RTP/AVP 96\nc=IN IP4 239.1.1.1\na=rtpmap:96 raw/90000\n"
                                "a=fmtp:96 sampling=" +
                                pf.sdpSampling() + "; width=1920; height=1080; exactframerate=50; depth=" +
                                std::to_string(pf.depth) + "; PM=2110GPM\n";
        auto session = parseSdp(sdp, error);
        REQUIRE(session);
        auto video = sdpVideo(*session, error);
        REQUIRE_MESSAGE(video, error);
        CHECK(video->format.pixel == pf);
        CHECK(video->packing == Packing::Gpm);
    }
}

TEST_CASE("Pixelgruppen nach RFC 4175 / MTL (st20_pgroups)") {
    struct Row {
        Sampling s;
        int d;
        uint32_t bytes;
        uint32_t pixels;
    };
    // Werte aus lib/src/st2110/st_fmt.c von MTL v26.09
    const Row rows[] = {
        {Sampling::Ycbcr422, 8, 4, 2},  {Sampling::Ycbcr422, 10, 5, 2},  {Sampling::Ycbcr422, 12, 6, 2},
        {Sampling::Ycbcr422, 16, 8, 2}, {Sampling::Ycbcr420, 8, 6, 4},   {Sampling::Ycbcr420, 10, 15, 8},
        {Sampling::Ycbcr420, 12, 9, 4}, {Sampling::Ycbcr420, 16, 12, 4}, {Sampling::Rgb, 8, 3, 1},
        {Sampling::Rgb, 10, 15, 4},     {Sampling::Rgb, 12, 9, 2},       {Sampling::Rgb, 16, 6, 1},
        {Sampling::Ycbcr444, 8, 3, 1},  {Sampling::Ycbcr444, 10, 15, 4}, {Sampling::Ycbcr444, 12, 9, 2},
        {Sampling::Ycbcr444, 16, 6, 1},
    };
    for (const auto& r : rows) {
        const PixelFormat pf{r.s, r.d};
        CHECK_MESSAGE(pf.pgroupBytes() == r.bytes, pf.name());
        CHECK_MESSAGE(pf.pgroupPixels() == r.pixels, pf.name());
    }
    CHECK(allPixelFormats().size() == 16);
    CHECK(VideoFormat{}.name() == "1920x1080p50");
    VideoFormat rgb;
    rgb.pixel = PixelFormat{Sampling::Rgb, 12};
    CHECK(rgb.name() == "1920x1080p50 RGB 12 Bit");
}
