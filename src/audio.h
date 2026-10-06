#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// 모든 오디오는 mono / 48kHz / float32 로 다룬다.
constexpr int kSampleRate = 48000;

// 마이크를 계속 켜 두고 들어오는 소리를 꺼내 가는 입력. 실시간 음정 표시용.
class LiveInput {
public:
    LiveInput();
    ~LiveInput();
    void start();                        // 실패 시 예외
    void stop();
    bool active() const;
    std::vector<float> drain();          // 마지막 drain 이후 들어온 샘플 (UI 스레드에서 매 프레임 호출)
    float level() const;                 // 최근 입력 피크 0~1

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 안내음: 주파수를 바꿔 가며 계속 울리는 부드러운 톤 (목표 음 들려주기). hz 가 0 이면 조용히.
class ToneGenerator {
public:
    ToneGenerator();
    ~ToneGenerator();
    void start();
    void stop();
    void set(float hz, float gain);      // 언제든 호출 가능 (클릭 없이 부드럽게 바뀐다)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace audio {
std::vector<float> resample(const std::vector<float>& pcm, int fromRate, int toRate);  // 선형 보간
std::vector<float> loadWav(const std::string& path, int sampleRate = kSampleRate);
void saveWav(const std::string& path, const std::vector<float>& pcm, int sampleRate = kSampleRate);
}  // namespace audio
