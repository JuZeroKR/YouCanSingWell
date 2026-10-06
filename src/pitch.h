#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 음높이(F0) 추출. 외부 라이브러리 없이 NCCF(정규화 자기상관) 로 10 ms 마다 한 값을 낸다.
// 실시간(마이크) 과 파일(노래 보컬) 둘 다 같은 분석기를 쓴다. 분석은 16 kHz 에서 한다.
namespace pitch {

constexpr int kRate = 16000;
constexpr int kHop = 160;          // 10 ms
constexpr int kHopMs = 10;
constexpr float kMinHz = 60.f, kMaxHz = 1000.f;  // 노래 음역 (C2 ≈ 65 Hz … B5 ≈ 988 Hz)

struct Frame {
    float hz = 0.f;        // 0 이면 무성음
    float clarity = 0.f;   // 주기성 0~1 (높을수록 또렷한 음)
    float db = -100.f;     // 프레임 RMS (dBFS)
    bool voiced = false;
};

// 반음 번호 (MIDI). A4 = 69 = 440 Hz
float hzToMidi(float hz);
float midiToHz(float midi);
std::string noteName(int midi);                  // "A4", "C#3"
std::string describe(float midi);                // "A4 +12" (가장 가까운 음과 센트 차이)

// 스트리밍 분석기: 48 kHz 샘플을 넣으면 10 ms 프레임이 나온다. 필터 · 창 상태를 유지한다.
class Tracker {
public:
    Tracker();
    void reset();
    // pcm48k 를 넣고 새로 완성된 프레임들을 돌려준다
    std::vector<Frame> push(const float* pcm48k, size_t n);
    // 파일 전체(16 kHz) 를 한 번에 분석
    static std::vector<Frame> analyzeAll(const std::vector<float>& pcm16k);

private:
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; float process(float x); };
    static Biquad makeBiquad(float fc, float fs, bool highpass);
    Frame analyzeFrame(const float* raw, const float* filt);   // 587 샘플
    void postProcess(Frame& f);

    Biquad hp_, lp_;
    std::vector<float> raw_, filt_;   // 16 kHz 로 내린 뒤의 입력 (원본 · 음높이용 필터 통과)
    float decimPhase_ = 0.f;          // 48k → 16k (3:1) 평균 디시메이션 상태
    float decimAcc_ = 0.f;
    int decimCount_ = 0;
    float noiseDb_ = -60.f;           // 바닥 소음 추정 (천천히 올라가고 빨리 내려간다)
    float lastHz_ = 0.f;              // 옥타브 튐 방지용 직전 값
    int voicedRun_ = 0;
    int unvoicedRun_ = 0;
};

}  // namespace pitch
