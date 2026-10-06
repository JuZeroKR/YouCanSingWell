#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pitch.h"

// 노래 가져오기 파이프라인: 유튜브 → mix.wav → 보컬 분리(ycs_separate, demucs) → 보컬 음정 선(melody.json)
// 모두 PC 안에서 돌고 AI 서비스 호출은 없다. 각 단계는 결과 파일이 이미 있으면 건너뛴다.
namespace song {

std::optional<std::string> extractVideoId(const std::string& input);
std::string dir(const std::string& id);          // <dataDir>/<id>
std::string modelPath();                           // <modelsDir>/ggml-model-htdemucs-4s-f16.bin
std::string modelUrl();
bool modelInstalled();
std::string logPath();

struct Info {
    std::string id, title, dir;
    bool separated = false;   // vocals.wav 가 있다
    bool melody = false;      // melody.json 이 있다
};
std::vector<Info> library();                       // 받아 둔 노래들 (최근 것부터)
Info info(const std::string& id);

using Status = std::function<void(const std::string&)>;

// 1) yt-dlp 로 소리만 받아 ffmpeg 로 44.1 kHz 스테레오 wav 를 만든다. title.txt 도 쓴다. 실패하면 예외
void download(const std::string& id, const Status& status);
// 2) 분리 모델이 없으면 받는다 (약 80MB)
void ensureModel(const Status& status);
// 3) ycs_separate 를 돌려 vocals.wav / no_vocals.wav 를 만든다. 진행률은 로그 파일을 읽는 쪽(UI) 에서 본다
void separate(const std::string& id, const Status& status);
// 4) vocals.wav 의 음높이를 10 ms 마다 뽑아 melody.json 으로 저장하고 돌려준다
std::vector<pitch::Frame> analyzeMelody(const std::string& id, const Status& status);
std::vector<pitch::Frame> loadMelody(const std::string& id);

// 로그 파일 끝의 "progress N" 을 읽는다 (분리 진행률, 없으면 -1)
int separationProgress();

}  // namespace song
