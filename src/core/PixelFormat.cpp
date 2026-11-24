#include "core/PixelFormat.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace st2110 {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

uint32_t PixelFormat::samplesPerBlock() const {
    switch (sampling) {
    case Sampling::Ycbcr422:
        return 4;
    case Sampling::Ycbcr420:
        return 6;
    case Sampling::Ycbcr444:
    case Sampling::Rgb:
        break;
    }
    return 3;
}

uint32_t PixelFormat::pixelsPerBlock() const {
    switch (sampling) {
    case Sampling::Ycbcr422:
        return 2;
    case Sampling::Ycbcr420:
        return 4;
    case Sampling::Ycbcr444:
    case Sampling::Rgb:
        break;
    }
    return 1;
}

uint32_t PixelFormat::pgroupBlocks() const {
    const uint32_t bits = samplesPerBlock() * static_cast<uint32_t>(depth);
    return 8 / std::gcd(bits, 8u);
}

uint32_t PixelFormat::pgroupBytes() const {
    return pgroupBlocks() * samplesPerBlock() * static_cast<uint32_t>(depth) / 8;
}

uint32_t PixelFormat::pgroupPixels() const {
    return pgroupBlocks() * pixelsPerBlock();
}

const char* samplingName(Sampling sampling) {
    switch (sampling) {
    case Sampling::Ycbcr422:
        return "YCbCr-4:2:2";
    case Sampling::Ycbcr420:
        return "YCbCr-4:2:0";
    case Sampling::Ycbcr444:
        return "YCbCr-4:4:4";
    case Sampling::Rgb:
        break;
    }
    return "RGB";
}

std::string PixelFormat::sdpSampling() const {
    return samplingName(sampling);
}

std::string PixelFormat::name() const {
    return sdpSampling() + " " + std::to_string(depth) + " Bit";
}

std::optional<Sampling> parseSampling(const std::string& text) {
    const std::string t = lower(text);
    if (t == "ycbcr-4:2:2" || t == "422" || t == "4:2:2") {
        return Sampling::Ycbcr422;
    }
    if (t == "ycbcr-4:2:0" || t == "420" || t == "4:2:0") {
        return Sampling::Ycbcr420;
    }
    if (t == "ycbcr-4:4:4" || t == "444" || t == "4:4:4") {
        return Sampling::Ycbcr444;
    }
    if (t == "rgb") {
        return Sampling::Rgb;
    }
    return std::nullopt;
}

bool validDepth(int depth) {
    return depth == 8 || depth == 10 || depth == 12 || depth == 16;
}

const std::vector<PixelFormat>& allPixelFormats() {
    static const std::vector<PixelFormat> all = [] {
        std::vector<PixelFormat> v;
        for (Sampling s : {Sampling::Ycbcr422, Sampling::Ycbcr420, Sampling::Ycbcr444, Sampling::Rgb}) {
            for (int d : {8, 10, 12, 16}) {
                v.push_back(PixelFormat{s, d});
            }
        }
        return v;
    }();
    return all;
}

const char* packingName(Packing packing) {
    switch (packing) {
    case Packing::Bpm:
        return "BPM";
    case Packing::Gpm:
        return "GPM";
    case Packing::GpmSl:
        break;
    }
    return "GPM_SL";
}

std::optional<Packing> parsePacking(const std::string& text) {
    const std::string t = lower(text);
    if (t == "bpm" || t == "2110bpm") {
        return Packing::Bpm;
    }
    if (t == "gpm" || t == "2110gpm") {
        return Packing::Gpm;
    }
    if (t == "gpm_sl" || t == "gpmsl") {
        return Packing::GpmSl;
    }
    return std::nullopt;
}

} // namespace st2110
