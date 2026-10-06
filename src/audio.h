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

// 노래 따라 부르기용 입출력 장치 하나: 반주 + 원곡 보컬을 재생하면서 마이크를 받아 (모니터링으로) 함께 내보낸다.
// 재생과 마이크가 같은 콜백에서 돌아 서로의 시각이 맞는다. 모든 버퍼는 mono 48 kHz.
class SongEngine {
public:
    SongEngine();
    ~SongEngine();
    void load(std::vector<float> inst48k, std::vector<float> vocal48k);  // 정지 상태에서 호출
    void start();                        // 장치 열기 (실패 시 예외)
    void stop();                         // 장치 닫기
    bool active() const;

    void play();
    void pause();
    bool playing() const;
    void seek(double sec);
    double positionSec() const;
    double durationSec() const;
    void setGains(float inst, float vocal, float monitor);

    // 마이크 샘플과 그때의 노래 위치(초). 콜백마다 한 덩어리
    struct MicChunk { double songSec; std::vector<float> pcm; };
    std::vector<MicChunk> drainMic();
    float micLevel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace audio {
std::vector<float> resample(const std::vector<float>& pcm, int fromRate, int toRate);  // 선형 보간
std::vector<float> loadWav(const std::string& path, int sampleRate = kSampleRate);
void saveWav(const std::string& path, const std::vector<float>& pcm, int sampleRate = kSampleRate);
}  // namespace audio
