#include "core/Preview.h"

#include <algorithm>
#include <array>
#include <cmath>

// JPEG-Kodierer (gemeinfrei, nothings/stb); nur die Callback-Ausgabe, kein Dateizugriff.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace st2110 {
namespace {

// BT.709, begrenzter Bereich (Y 64–940, C 64–960 bei 10 Bit) nach RGB 8 Bit, als Tabellen in
// Festkomma (16 Bit Nachkomma): R = Y + crR, G = Y + cbG + crG, B = Y + cbB.
struct ColorTables {
    std::array<int32_t, 1024> y{}, crR{}, cbG{}, crG{}, cbB{};
    ColorTables() {
        for (int v = 0; v < 1024; ++v) {
            const double yy = (v - 64) / 876.0 * 255.0;
            const double c = (v - 512) / 896.0 * 255.0;
            y[v] = static_cast<int32_t>(std::lround(yy * 65536.0));
            crR[v] = static_cast<int32_t>(std::lround(1.5748 * c * 65536.0));
            cbG[v] = static_cast<int32_t>(std::lround(-0.1873 * c * 65536.0));
            crG[v] = static_cast<int32_t>(std::lround(-0.4681 * c * 65536.0));
            cbB[v] = static_cast<int32_t>(std::lround(1.8556 * c * 65536.0));
        }
    }
};

const ColorTables& tables() {
    static const ColorTables t;
    return t;
}

inline uint8_t clampFixed(int32_t v) {
    v = (v + 32768) >> 16;
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

inline void yuvToRgb(const ColorTables& t, int y, int cb, int cr, uint8_t* rgb) {
    const int32_t yy = t.y[y];
    rgb[0] = clampFixed(yy + t.crR[cr]);
    rgb[1] = clampFixed(yy + t.cbG[cb] + t.crG[cr]);
    rgb[2] = clampFixed(yy + t.cbB[cb]);
}

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::string*>(context);
    out->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

} // namespace

bool encodeJpeg(PreviewImage& image, int quality) {
    if (image.width == 0 || image.height == 0 ||
        image.rgb.size() < static_cast<size_t>(image.width) * image.height * 3) {
        return false;
    }
    image.jpeg.clear();
    image.jpeg.reserve(static_cast<size_t>(image.width) * image.height / 4);
    return stbi_write_jpg_to_func(&appendBytes, &image.jpeg, static_cast<int>(image.width),
                                  static_cast<int>(image.height), 3, image.rgb.data(),
                                  std::clamp(quality, 10, 95)) != 0;
}

bool convertRfc4175ToRgb(const uint8_t* frame, size_t frameBytes, uint32_t width, uint32_t lines, bool field,
                         uint32_t maxWidth, PreviewImage& out) {
    if (frame == nullptr || width < 2 || width % 2 != 0 || lines == 0 || maxWidth < 2) {
        return false;
    }
    const size_t lineBytes = static_cast<size_t>(width) / 2 * 5;
    if (frameBytes < lineBytes * lines) {
        return false;
    }
    // Schrittweite in Pixeln; gerade, damit je Schritt ganze Pixelpaare entfallen.
    uint32_t step = std::max<uint32_t>(1, (width + maxWidth - 1) / maxWidth);
    if (step > 1 && step % 2 != 0) {
        ++step;
    }
    const uint32_t pairStep = std::max<uint32_t>(1, step / 2);
    const uint32_t pairs = width / 2;
    const uint32_t outPairs = (pairs + pairStep - 1) / pairStep;
    // Senkrecht: volle Bildhöhe (bei Halbbildern doppelt) durch dieselbe Schrittweite
    const uint32_t fullLines = field ? lines * 2 : lines;
    out.width = outPairs * (step >= 2 ? 1 : 2);
    out.height = (fullLines + step - 1) / step;
    out.rgb.assign(static_cast<size_t>(out.width) * out.height * 3, 0);
    const ColorTables& t = tables();
    for (uint32_t oy = 0; oy < out.height; ++oy) {
        const uint32_t srcLine = std::min(lines - 1, (oy * step) / (field ? 2 : 1));
        const uint8_t* line = frame + static_cast<size_t>(srcLine) * lineBytes;
        uint8_t* dst = out.rgb.data() + static_cast<size_t>(oy) * out.width * 3;
        for (uint32_t op = 0; op < outPairs; ++op) {
            const uint8_t* g = line + static_cast<size_t>(op) * pairStep * 5;
            // 40 Bit: Cb(10) Y0(10) Cr(10) Y1(10)
            const int cb = (g[0] << 2) | (g[1] >> 6);
            const int y0 = ((g[1] & 0x3F) << 4) | (g[2] >> 4);
            const int cr = ((g[2] & 0x0F) << 6) | (g[3] >> 2);
            const int y1 = ((g[3] & 0x03) << 8) | g[4];
            if (step >= 2) {
                yuvToRgb(t, (y0 + y1) / 2, cb, cr, dst);
                dst += 3;
            } else {
                yuvToRgb(t, y0, cb, cr, dst);
                yuvToRgb(t, y1, cb, cr, dst + 3);
                dst += 6;
            }
        }
    }
    return true;
}

void renderTestPattern(PreviewImage& out, uint32_t width, uint32_t height, double t, int variant,
                       bool glitch) {
    out.width = width;
    out.height = height;
    out.rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    // 75-%-Farbbalken: Weiß, Gelb, Cyan, Grün, Magenta, Rot, Blau
    static const uint8_t kBars[7][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
                                        {191, 0, 191},   {191, 0, 0},   {0, 0, 191}};
    const uint32_t barsEnd = height * 2 / 3;
    const uint32_t rampEnd = height * 5 / 6;
    const int rotate = ((variant % 7) + 7) % 7;
    const uint32_t box = std::max<uint32_t>(4, height / 6);
    const double phase = std::fmod(t * 0.25, 1.0);
    const uint32_t boxX = static_cast<uint32_t>(phase * (width - box));
    const uint32_t boxY = barsEnd + (rampEnd - barsEnd - std::min(box, rampEnd - barsEnd)) / 2;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = out.rgb.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = row + static_cast<size_t>(x) * 3;
            if (y < barsEnd) {
                const auto& c = kBars[(x * 7 / width + rotate) % 7];
                p[0] = c[0];
                p[1] = c[1];
                p[2] = c[2];
            } else if (y < rampEnd) {
                const auto v = static_cast<uint8_t>(16 + (219 * x) / std::max<uint32_t>(1, width - 1));
                p[0] = p[1] = p[2] = v;
            } else {
                // Unten: dunkle Fläche mit Sekunden-Blitz
                const bool flash = std::fmod(t, 1.0) < 0.08 && x > width * 9 / 10;
                p[0] = p[1] = p[2] = flash ? 235 : 16;
            }
            if (x >= boxX && x < boxX + box && y >= boxY && y < boxY + box) {
                p[0] = 235;
                p[1] = 64;
                p[2] = 32;
            }
        }
    }
    if (glitch) {
        const auto line = static_cast<uint32_t>(std::fmod(t * 997.0, 1.0) * height);
        for (uint32_t y = line; y < std::min(height, line + 2); ++y) {
            std::fill_n(out.rgb.data() + static_cast<size_t>(y) * width * 3, width * 3, 0);
        }
    }
}

void VideoPreview::request(double now, const PreviewSettings& settings) {
    std::lock_guard lock(mutex_);
    requestedAt_ = now;
    settings_ = settings;
    settings_.fps = std::clamp(settings_.fps, 0.2, kMaxFps);
    settings_.maxWidth = std::clamp<uint32_t>(settings_.maxWidth, 64, 8192);
    settings_.quality = std::clamp(settings_.quality, 10, 95);
}

PreviewSettings VideoPreview::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

bool VideoPreview::due(double now) const {
    std::lock_guard lock(mutex_);
    // lastPut_ ist der Rasterpunkt des letzten Bildes; das nächste ist ein Intervall später fällig.
    return now - requestedAt_ < kKeepAliveSec && now >= lastPut_ + 1.0 / settings_.fps - 1e-6;
}

void VideoPreview::put(PreviewImage image, double now) {
    {
        std::lock_guard lock(mutex_);
        image.sequence = ++sequence_;
        image.timeSec = now;
        image_ = std::move(image);
        // Festes Raster der Bildrate: kleine Verspätungen holen die folgenden Bilder wieder auf,
        // nach einer Pause beginnt das Raster neu.
        const double interval = 1.0 / settings_.fps;
        lastPut_ = now - lastPut_ < 2 * interval ? lastPut_ + interval : now;
    }
    newImage_.notify_all();
}

PreviewImage VideoPreview::waitNewer(uint64_t afterSequence, double timeoutSec) const {
    std::unique_lock lock(mutex_);
    newImage_.wait_for(lock, std::chrono::duration<double>(timeoutSec),
                       [&] { return sequence_ > afterSequence; });
    return image_;
}

PreviewImage VideoPreview::latest() const {
    std::lock_guard lock(mutex_);
    return image_;
}

void AudioPreview::configure(uint32_t sampleRate, uint16_t channels) {
    std::lock_guard lock(mutex_);
    sampleRate_ = sampleRate;
    channels_ = static_cast<uint16_t>(std::clamp<int>(channels, 1, static_cast<int>(kMaxChannels)));
    capacity_ = static_cast<size_t>(sampleRate_) * 2; // 2 s
    ring_.assign(capacity_ * channels_, 0.0f);
    written_ = 0;
    peak_.fill(0.0f);
}

void AudioPreview::request(double now) {
    std::lock_guard lock(mutex_);
    requestedAt_ = now;
}

bool AudioPreview::wanted(double now) const {
    std::lock_guard lock(mutex_);
    return now - requestedAt_ < kKeepAliveSec;
}

void AudioPreview::pushPcm(const uint8_t* data, size_t bytes, uint8_t bitDepth) {
    const size_t bytesPerSample = bitDepth == 16 ? 2 : 3;
    std::lock_guard lock(mutex_);
    if (capacity_ == 0) {
        return;
    }
    const size_t frames = bytes / (bytesPerSample * channels_);
    for (size_t f = 0; f < frames; ++f) {
        float* dst = ring_.data() + ((written_ + f) % capacity_) * channels_;
        for (uint16_t c = 0; c < channels_; ++c) {
            const uint8_t* s = data + (f * channels_ + c) * bytesPerSample;
            float v;
            if (bytesPerSample == 2) {
                v = static_cast<float>(static_cast<int16_t>((s[0] << 8) | s[1])) / 32768.0f;
            } else {
                int32_t x = (s[0] << 16) | (s[1] << 8) | s[2];
                if ((x & 0x800000) != 0) {
                    x -= 0x1000000;
                }
                v = static_cast<float>(x) / 8388608.0f;
            }
            dst[c] = v;
            peak_[c] = std::max(peak_[c], std::fabs(v));
        }
    }
    written_ += frames;
}

void AudioPreview::pushFloat(const float* samples, size_t frames) {
    std::lock_guard lock(mutex_);
    if (capacity_ == 0) {
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        float* dst = ring_.data() + ((written_ + f) % capacity_) * channels_;
        for (uint16_t c = 0; c < channels_; ++c) {
            dst[c] = samples[f * channels_ + c];
            peak_[c] = std::max(peak_[c], std::fabs(dst[c]));
        }
    }
    written_ += frames;
}

AudioPreview::Chunk AudioPreview::read(uint64_t from, unsigned pair, uint32_t maxFrames, double leadSec) {
    std::lock_guard lock(mutex_);
    Chunk c;
    c.sampleRate = sampleRate_;
    c.channels = channels_;
    c.peaksDbfs.resize(channels_);
    for (uint16_t ch = 0; ch < channels_; ++ch) {
        c.peaksDbfs[ch] = peak_[ch] > 1e-6f ? 20.0f * std::log10(peak_[ch]) : -120.0f;
        peak_[ch] = 0.0f;
    }
    const uint64_t oldest = written_ > capacity_ ? written_ - capacity_ : 0;
    const auto lead = static_cast<uint64_t>(leadSec * sampleRate_);
    if (from == 0 || from < oldest || from > written_) {
        from = written_ > lead ? written_ - lead : 0;
    }
    c.start = from;
    c.frames = static_cast<uint32_t>(std::min<uint64_t>(written_ - from, maxFrames));
    const unsigned left = std::min<unsigned>(pair * 2, channels_ - 1u);
    const unsigned right = std::min<unsigned>(left + 1, channels_ - 1u);
    c.stereo.resize(static_cast<size_t>(c.frames) * 2);
    for (uint32_t f = 0; f < c.frames; ++f) {
        const float* src = ring_.data() + ((from + f) % capacity_) * channels_;
        c.stereo[f * 2] = static_cast<int16_t>(std::clamp(src[left], -1.0f, 1.0f) * 32767.0f);
        c.stereo[f * 2 + 1] = static_cast<int16_t>(std::clamp(src[right], -1.0f, 1.0f) * 32767.0f);
    }
    return c;
}

void generateTestTone(std::vector<float>& out, uint64_t sampleIndex, size_t frames, uint16_t channels,
                      uint32_t sampleRate) {
    out.resize(frames * channels);
    const double amplitude = std::pow(10.0, -18.0 / 20.0);
    constexpr double kTwoPi = 6.283185307179586;
    for (size_t f = 0; f < frames; ++f) {
        const uint64_t n = sampleIndex + f;
        const double t = static_cast<double>(n) / sampleRate;
        for (uint16_t c = 0; c < channels; ++c) {
            const double hz = c == 0 ? 1000.0 : c == 1 ? 2000.0 : 500.0 * (c + 1);
            double v = amplitude * std::sin(kTwoPi * std::fmod(hz * t, 1.0));
            if (c == 0 && std::fmod(t, 3.0) < 0.25) {
                v = 0.0; // EBU-Kennung: links 250 ms Pause alle 3 s
            }
            out[f * channels + c] = static_cast<float>(v);
        }
    }
}

} // namespace st2110
