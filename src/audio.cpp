#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace {

ma_device_config makeConfig(ma_device_type type, ma_device_data_proc cb, void* user) {
    ma_device_config cfg = ma_device_config_init(type);
    cfg.sampleRate = kSampleRate;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = cb;
    cfg.pUserData = user;
    return cfg;
}

void openAndStart(ma_device& dev, const ma_device_config& cfg, const char* what) {
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) throw std::runtime_error(std::string(what) + " 장치를 열 수 없습니다");
    if (ma_device_start(&dev) != MA_SUCCESS) {
        ma_device_uninit(&dev);
        throw std::runtime_error(std::string(what) + " 장치를 시작할 수 없습니다");
    }
}

}  // namespace

// ---------------- LiveInput ----------------

struct LiveInput::Impl {
    ma_device dev{};
    bool running = false;
    std::mutex m;
    std::vector<float> buf;
    std::atomic<float> level{0.f};

    static void cb(ma_device* d, void*, const void* in, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(d->pUserData);
        const float* x = static_cast<const float*>(in);
        float peak = 0.f;
        for (ma_uint32 i = 0; i < frames; ++i) peak = std::max(peak, std::fabs(x[i]));
        self->level = std::max(peak, self->level.load() * 0.85f);
        std::lock_guard<std::mutex> lock(self->m);
        if (self->buf.size() < (size_t)kSampleRate * 10) self->buf.insert(self->buf.end(), x, x + frames);  // 10 초 넘게 안 가져가면 버린다
    }
};

LiveInput::LiveInput() : impl_(new Impl) {}
LiveInput::~LiveInput() { stop(); }

void LiveInput::start() {
    if (impl_->running) return;
    openAndStart(impl_->dev, makeConfig(ma_device_type_capture, Impl::cb, impl_.get()), "마이크");
    impl_->running = true;
}

void LiveInput::stop() {
    if (!impl_->running) return;
    ma_device_uninit(&impl_->dev);
    impl_->running = false;
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->buf.clear();
}

bool LiveInput::active() const { return impl_->running; }
float LiveInput::level() const { return impl_->level.load(); }

std::vector<float> LiveInput::drain() {
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<float> out;
    out.swap(impl_->buf);
    return out;
}

// ---------------- ToneGenerator ----------------

struct ToneGenerator::Impl {
    ma_device dev{};
    bool running = false;
    std::atomic<float> targetHz{0.f}, targetGain{0.f};
    float hz = 0.f, gain = 0.f;
    double phase = 0.0;

    static void cb(ma_device* d, void* out, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(d->pUserData);
        float* y = static_cast<float*>(out);
        const float th = self->targetHz.load(), tg = self->targetGain.load();
        for (ma_uint32 i = 0; i < frames; ++i) {
            // 주파수 · 음량을 천천히 따라가서 딱딱 끊기지 않게 한다
            self->hz += (th - self->hz) * 0.002f;
            self->gain += (tg - self->gain) * 0.001f;
            if (self->hz > 20.f) {
                self->phase += self->hz / kSampleRate;
                if (self->phase >= 1.0) self->phase -= 1.0;
            }
            // 기본음 + 약한 2 · 3 배음: 사인파보다 음높이를 알아듣기 쉽다
            const float p = (float)self->phase * 6.2831853f;
            y[i] = self->gain * (std::sin(p) + 0.35f * std::sin(2 * p) + 0.15f * std::sin(3 * p));
        }
    }
};

ToneGenerator::ToneGenerator() : impl_(new Impl) {}
ToneGenerator::~ToneGenerator() { stop(); }

void ToneGenerator::start() {
    if (impl_->running) return;
    openAndStart(impl_->dev, makeConfig(ma_device_type_playback, Impl::cb, impl_.get()), "스피커");
    impl_->running = true;
}

void ToneGenerator::stop() {
    if (!impl_->running) return;
    ma_device_uninit(&impl_->dev);
    impl_->running = false;
}

void ToneGenerator::set(float hz, float gain) {
    impl_->targetHz = hz;
    impl_->targetGain = gain;
}

// ---------------- SongEngine ----------------

struct SongEngine::Impl {
    ma_device dev{};
    bool running = false;
    std::vector<float> inst, vocal;
    std::atomic<size_t> pos{0};
    std::atomic<bool> playing{false};
    std::atomic<float> gInst{0.8f}, gVocal{0.3f}, gMon{0.7f};
    std::atomic<float> level{0.f};
    std::mutex m;
    std::vector<MicChunk> chunks;

    static void cb(ma_device* d, void* out, const void* in, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(d->pUserData);
        float* y = static_cast<float*>(out);
        const float* x = static_cast<const float*>(in);
        const float gi = self->gInst.load(), gv = self->gVocal.load(), gm = self->gMon.load();
        size_t p = self->pos.load();
        const bool play = self->playing.load();
        const size_t n = self->inst.size();
        MicChunk chunk;
        chunk.songSec = (double)p / kSampleRate;
        chunk.pcm.assign(x, x + frames);
        float peak = 0.f;
        for (ma_uint32 i = 0; i < frames; ++i) {
            float s = x[i] * gm;
            peak = std::max(peak, std::fabs(x[i]));
            if (play && p < n) {
                s += self->inst[p] * gi + (p < self->vocal.size() ? self->vocal[p] * gv : 0.f);
                ++p;
            }
            y[i] = std::max(-1.f, std::min(1.f, s));
        }
        if (play) {
            self->pos.store(p);
            if (p >= n) self->playing.store(false);  // 끝
        }
        self->level = std::max(peak, self->level.load() * 0.85f);
        std::lock_guard<std::mutex> lock(self->m);
        if (self->chunks.size() < 2000) self->chunks.push_back(std::move(chunk));
    }
};

SongEngine::SongEngine() : impl_(new Impl) {}
SongEngine::~SongEngine() { stop(); }

void SongEngine::load(std::vector<float> inst48k, std::vector<float> vocal48k) {
    const bool was = impl_->running;
    if (was) stop();
    impl_->inst = std::move(inst48k);
    impl_->vocal = std::move(vocal48k);
    impl_->pos = 0;
    impl_->playing = false;
    if (was) start();
}

void SongEngine::start() {
    if (impl_->running) return;
    openAndStart(impl_->dev, makeConfig(ma_device_type_duplex, Impl::cb, impl_.get()), "오디오");
    impl_->running = true;
}

void SongEngine::stop() {
    if (!impl_->running) return;
    impl_->playing = false;
    ma_device_uninit(&impl_->dev);
    impl_->running = false;
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->chunks.clear();
}

bool SongEngine::active() const { return impl_->running; }
void SongEngine::play() { if (impl_->pos.load() >= impl_->inst.size()) impl_->pos = 0; impl_->playing = true; }
void SongEngine::pause() { impl_->playing = false; }
bool SongEngine::playing() const { return impl_->playing.load(); }
void SongEngine::seek(double sec) {
    const size_t p = (size_t)std::max(0.0, sec) * kSampleRate;
    impl_->pos = std::min(p, impl_->inst.size());
}
double SongEngine::positionSec() const { return (double)impl_->pos.load() / kSampleRate; }
double SongEngine::durationSec() const { return (double)impl_->inst.size() / kSampleRate; }
void SongEngine::setGains(float inst, float vocal, float monitor) { impl_->gInst = inst; impl_->gVocal = vocal; impl_->gMon = monitor; }
float SongEngine::micLevel() const { return impl_->level.load(); }

std::vector<SongEngine::MicChunk> SongEngine::drainMic() {
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<MicChunk> out;
    out.swap(impl_->chunks);
    return out;
}

// ---------------- 파일 · 변환 ----------------

namespace audio {

std::vector<float> resample(const std::vector<float>& pcm, int fromRate, int toRate) {
    if (fromRate == toRate || pcm.empty()) return pcm;
    const double ratio = (double)fromRate / toRate;
    std::vector<float> out((size_t)(pcm.size() / ratio));
    for (size_t i = 0; i < out.size(); ++i) {
        double pos = i * ratio;
        size_t k = (size_t)pos;
        double f = pos - k;
        float a = pcm[std::min(k, pcm.size() - 1)];
        float b = pcm[std::min(k + 1, pcm.size() - 1)];
        out[i] = (float)(a + (b - a) * f);
    }
    return out;
}

std::vector<float> loadWav(const std::string& path, int sampleRate) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)sampleRate);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) throw std::runtime_error("오디오 파일을 열 수 없음: " + path);
    std::vector<float> pcm, chunk((size_t)sampleRate);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size(), &got);
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + (size_t)got);
        if (r != MA_SUCCESS || got < chunk.size()) break;
    }
    ma_decoder_uninit(&dec);
    return pcm;
}

void saveWav(const std::string& path, const std::vector<float>& pcm, int sampleRate) {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, (ma_uint32)sampleRate);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) throw std::runtime_error("파일을 저장할 수 없음: " + path);
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, pcm.data(), pcm.size(), &written);
    ma_encoder_uninit(&enc);
}

}  // namespace audio
