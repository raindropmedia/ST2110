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
                 "a=fmtp:96 sampling=YCbCr-4:2:2; width=1920; height=1080; exactframerate=50; depth=8\n",
                 error);
    REQUIRE(session);
    CHECK_FALSE(sdpVideo(*session, error));
    CHECK(error.find("10 Bit") != std::string::npos);
    CHECK_FALSE(parseSdp("kein sdp", error));
}

TEST_CASE("Videoformat und exakte Bildrate") {
    CHECK(parseVideoFormat("3840x2160p59.94")->rate == FrameRate::P59_94);
    CHECK_FALSE(parseVideoFormat("1920x1080x50"));
    CHECK(parseExactFrameRate("24000/1001") == FrameRate::P23_98);
    CHECK(parseExactFrameRate("25") == FrameRate::P25);
    CHECK_FALSE(parseExactFrameRate("48"));
}
