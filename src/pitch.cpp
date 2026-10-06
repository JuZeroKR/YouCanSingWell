#include "pitch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pitch {
namespace {

constexpr int kCorrWin = 320;                      // 상관 창 20 ms
constexpr int kLagMin = (int)(kRate / kMaxHz);     // 16  (1000 Hz)
constexpr int kLagMax = (int)(kRate / kMinHz);     // 266 (60 Hz)
constexpr int kFrameLen = kCorrWin + kLagMax;      // 586
constexpr float kPi = 3.14159265358979f;
constexpr float kClarityVoiced = 0.62f;            // 주기성 문턱 (너무 낮으면 방 소음이 점으로 깜빡인다)
constexpr float kAboveNoiseDb = 15.f;              // 바닥 소음보다 이만큼 커야 소리로 친다
constexpr float kLagBias = 0.03f;                  // 긴 지연(낮은 음) 을 조금 불리하게 — 옥타브 아래로 떨어지는 것 방지

const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

}  // namespace

float hzToMidi(float hz) { return hz > 0.f ? 69.f + 12.f * std::log2(hz / 440.f) : 0.f; }
float midiToHz(float midi) { return 440.f * std::pow(2.f, (midi - 69.f) / 12.f); }

std::string noteName(int midi) {
    const int n = ((midi % 12) + 12) % 12;
    const int octave = midi / 12 - 1;
    return std::string(kNames[n]) + std::to_string(octave);
}

std::string describe(float midi) {
    const int nearest = (int)std::lround(midi);
    const int cents = (int)std::lround((midi - nearest) * 100.f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s %+d", noteName(nearest).c_str(), cents);
    return buf;
}

float Tracker::Biquad::process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

Tracker::Biquad Tracker::makeBiquad(float fc, float fs, bool highpass) {
    const float q = 0.70710678f;
    const float w0 = 2.f * kPi * fc / fs, c = std::cos(w0), s = std::sin(w0), alpha = s / (2.f * q);
    const float a0 = 1.f + alpha;
    Biquad f;
    if (highpass) { f.b0 = (1.f + c) * 0.5f / a0; f.b1 = -(1.f + c) / a0; f.b2 = (1.f + c) * 0.5f / a0; }
    else { f.b0 = (1.f - c) * 0.5f / a0; f.b1 = (1.f - c) / a0; f.b2 = (1.f - c) * 0.5f / a0; }
    f.a1 = -2.f * c / a0;
    f.a2 = (1.f - alpha) / a0;
    return f;
}

Tracker::Tracker() { reset(); }

void Tracker::reset() {
    hp_ = makeBiquad(50.f, (float)kRate, true);
    lp_ = makeBiquad(1200.f, (float)kRate, false);
    raw_.clear();
    filt_.clear();
    decimAcc_ = 0.f;
    decimCount_ = 0;
    noiseDb_ = -60.f;
    lastHz_ = 0.f;
    voicedRun_ = 0;
    unvoicedRun_ = 0;
}

Frame Tracker::analyzeFrame(const float* raw, const float* filt) {
    Frame f;
    // 크기: 앞 320 샘플 RMS
    double e = 0;
    for (int i = 0; i < kCorrWin; ++i) e += (double)raw[i] * raw[i];
    f.db = 20.f * std::log10((float)std::sqrt(e / kCorrWin) + 1e-6f);

    // NCCF
    double mean = 0;
    for (int i = 0; i < kFrameLen; ++i) mean += filt[i];
    mean /= kFrameLen;
    static thread_local std::vector<float> x;
    static thread_local std::vector<double> prefix;
    x.resize(kFrameLen);
    prefix.resize(kFrameLen + 1);
    prefix[0] = 0;
    for (int i = 0; i < kFrameLen; ++i) {
        x[i] = (float)(filt[i] - mean);
        prefix[i + 1] = prefix[i] + (double)x[i] * x[i];
    }
    const double e0 = prefix[kCorrWin];
    if (e0 <= 1e-12) return f;
    static thread_local std::vector<float> corr;
    corr.assign(kLagMax + 1, 0.f);
    int best = -1;
    float bestScore = -1e9f;
    for (int lag = kLagMin; lag <= kLagMax; ++lag) {
        float num = 0.f;
        const float* a = x.data();
        const float* b = x.data() + lag;
        for (int i = 0; i < kCorrWin; ++i) num += a[i] * b[i];
        const double eLag = prefix[lag + kCorrWin] - prefix[lag];
        const float r = (float)(num / std::sqrt(e0 * eLag + 1e-12));
        corr[lag] = r;
        const float score = r - kLagBias * (float)lag / (float)kLagMax;
        if (score > bestScore) { bestScore = score; best = lag; }
    }
    if (best < 0) return f;
    float delta = 0.f;
    if (best > kLagMin && best < kLagMax) {
        const float a = corr[best - 1], b = corr[best], c = corr[best + 1];
        const float denom = a - 2.f * b + c;
        if (denom < -1e-9f) delta = std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / denom));
    }
    f.clarity = std::max(0.f, corr[best]);
    f.hz = (float)kRate / ((float)best + delta);
    return f;
}

void Tracker::postProcess(Frame& f) {
    // 바닥 소음: 조용할 땐 천천히 따라 내려가고, 커지면 아주 천천히 올라간다
    if (f.db < noiseDb_) noiseDb_ += (f.db - noiseDb_) * 0.2f;
    else noiseDb_ += (f.db - noiseDb_) * 0.002f;
    const bool loud = f.db > noiseDb_ + kAboveNoiseDb && f.db > -48.f;
    f.voiced = loud && f.clarity >= kClarityVoiced && f.hz >= kMinHz && f.hz <= kMaxHz;
    if (f.voiced && lastHz_ > 0.f) {
        // 직전 값의 옥타브 위아래로 튀었으면 바로잡는다 (한 프레임 만에 한 옥타브를 넘는 노래는 없다)
        const float ratio = f.hz / lastHz_;
        if (ratio > 1.8f && ratio < 2.2f) f.hz *= 0.5f;
        else if (ratio > 0.45f && ratio < 0.55f) f.hz *= 2.f;
    }
    if (f.voiced) { lastHz_ = f.hz; ++voicedRun_; unvoicedRun_ = 0; }
    else { f.hz = 0.f; voicedRun_ = 0; if (++unvoicedRun_ > 30) lastHz_ = 0.f; }  // 300 ms 쉬면 직전 값을 잊는다
}

std::vector<Frame> Tracker::push(const float* pcm48k, size_t n) {
    // 48k → 16k: 3 샘플 평균 (간단한 저역 통과 겸)
    for (size_t i = 0; i < n; ++i) {
        decimAcc_ += pcm48k[i];
        if (++decimCount_ == 3) {
            const float v = decimAcc_ / 3.f;
            decimAcc_ = 0.f;
            decimCount_ = 0;
            raw_.push_back(v);
            filt_.push_back(lp_.process(hp_.process(v)));
        }
    }
    std::vector<Frame> out;
    size_t pos = 0;
    while (raw_.size() - pos >= (size_t)kFrameLen) {
        Frame f = analyzeFrame(raw_.data() + pos, filt_.data() + pos);
        postProcess(f);
        out.push_back(f);
        pos += kHop;
    }
    if (pos > 0) {
        raw_.erase(raw_.begin(), raw_.begin() + pos);
        filt_.erase(filt_.begin(), filt_.begin() + pos);
    }
    return out;
}

std::vector<Frame> Tracker::analyzeAll(const std::vector<float>& pcm16k) {
    Tracker t;
    std::vector<float> filt(pcm16k.size());
    for (size_t i = 0; i < pcm16k.size(); ++i) filt[i] = t.lp_.process(t.hp_.process(pcm16k[i]));
    std::vector<Frame> out;
    // 파일은 바닥 소음을 전체 5 퍼센타일로 한 번에 정한다
    std::vector<float> dbs;
    for (size_t pos = 0; pos + kFrameLen <= pcm16k.size(); pos += kHop) {
        Frame f = t.analyzeFrame(pcm16k.data() + pos, filt.data() + pos);
        out.push_back(f);
        dbs.push_back(f.db);
    }
    if (dbs.empty()) return out;
    std::vector<float> sorted = dbs;
    std::sort(sorted.begin(), sorted.end());
    t.noiseDb_ = sorted[sorted.size() / 20];
    const float floorDb = t.noiseDb_;
    for (auto& f : out) {
        t.noiseDb_ = floorDb;
        t.postProcess(f);
    }
    // 3 프레임 미만의 유성음 섬 제거
    for (size_t i = 0; i < out.size();) {
        if (!out[i].voiced) { ++i; continue; }
        size_t j = i;
        while (j < out.size() && out[j].voiced) ++j;
        if (j - i < 3) for (size_t k = i; k < j; ++k) { out[k].voiced = false; out[k].hz = 0.f; }
        i = j;
    }
    return out;
}

}  // namespace pitch
