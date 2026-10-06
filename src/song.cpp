#include "song.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

#include "audio.h"
#include "json.hpp"
#include "paths.h"

namespace fs = std::filesystem;

namespace song {

namespace {

constexpr int kMelodyVersion = 3;  // melody.json 형식. 뽑는 규칙이 바뀌면 올린다 (옛 파일은 다시 뽑는다)

std::string readLine(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::string s;
    std::getline(in, s);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

bool exists(const std::string& p) { std::error_code ec; return fs::exists(fs::u8path(p), ec); }

}  // namespace

std::optional<std::string> extractVideoId(const std::string& input) {
    static const std::regex patterns[] = {
        std::regex(R"([?&]v=([A-Za-z0-9_-]{11}))"),
        std::regex(R"(youtu\.be/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/shorts/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/embed/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/live/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(^([A-Za-z0-9_-]{11})$)"),
    };
    for (const auto& re : patterns) {
        std::smatch m;
        if (std::regex_search(input, m, re)) return m[1].str();
    }
    return std::nullopt;
}

std::string dir(const std::string& id) { return paths::dataDir() + "/" + id; }
std::string modelPath() { return paths::modelsDir() + "/ggml-model-htdemucs-4s-f16.bin"; }
std::string modelUrl() { return "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-4s-f16.bin"; }
bool modelInstalled() { return exists(modelPath()); }
std::string logPath() { return paths::logDir() + "/tools.log"; }

Info info(const std::string& id) {
    Info i;
    i.id = id;
    i.dir = dir(id);
    i.title = readLine(i.dir + "/title.txt");
    if (i.title.empty()) i.title = id;
    i.separated = exists(i.dir + "/vocals.wav") && exists(i.dir + "/no_vocals.wav");
    i.melody = exists(i.dir + "/melody.json");
    return i;
}

std::vector<Info> library() {
    std::vector<std::pair<fs::file_time_type, Info>> items;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(paths::dataDir()), ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string id = e.path().filename().u8string();
        if (!exists(e.path().u8string() + "/mix.wav")) continue;
        items.push_back({fs::last_write_time(e.path(), ec), info(id)});
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Info> out;
    for (auto& it : items) out.push_back(std::move(it.second));
    return out;
}

void download(const std::string& id, const Status& status) {
    const std::string d = dir(id);
    fs::create_directories(fs::u8path(d));
    const std::string src = d + "/source.m4a", mix = d + "/mix.wav";
    if (!exists(mix)) {
        if (!exists(src)) {
            status("유튜브에서 소리 받는 중...");
            // 소리만 (m4a). 제목도 함께 저장
            std::string cmd = "yt-dlp --no-playlist -f \"ba[ext=m4a]/ba/b\" "
                              "--print-to-file \"%(title)s\" \"" + d + "/title.txt\" "
                              "-o \"" + d + "/source.%(ext)s\" "
                              "\"https://www.youtube.com/watch?v=" + id + "\"";
            int rc = paths::runCommand(cmd, logPath());
            // 확장자가 m4a 가 아닐 수도 있다 (webm 등): source.* 를 찾는다
            std::string found;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(fs::u8path(d), ec))
                if (e.path().stem() == "source") found = e.path().u8string();
            if (found.empty()) throw std::runtime_error("yt-dlp 로 소리를 받지 못했습니다 (exit code " + std::to_string(rc) + "). 로그: " + logPath());
            if (found != src) fs::rename(fs::u8path(found), fs::u8path(src), ec);
        }
        status("wav 로 변환 중...");
        std::string cmd = "ffmpeg -y -loglevel error -i \"" + src + "\" -vn -ac 2 -ar 44100 -f wav \"" + mix + "\"";
        int rc = paths::runCommand(cmd, logPath());
        if (rc != 0 || !exists(mix)) throw std::runtime_error("ffmpeg 변환 실패 (exit code " + std::to_string(rc) + "). 로그: " + logPath());
    }
}

void ensureModel(const Status& status) {
    if (modelInstalled()) return;
    status("보컬 분리 모델 받는 중 (약 80MB, 한 번만)...");
    fs::create_directories(fs::u8path(paths::modelsDir()));
    const std::string part = modelPath() + ".part";
    std::string cmd = "curl -L --fail -o \"" + part + "\" \"" + modelUrl() + "\"";
    if (paths::runCommand(cmd, logPath()) != 0 || !exists(part)) throw std::runtime_error("모델 다운로드 실패. 로그: " + logPath());
    std::error_code ec;
    fs::rename(fs::u8path(part), fs::u8path(modelPath()), ec);
    if (!modelInstalled()) throw std::runtime_error("모델 파일을 저장하지 못했습니다");
}

void separate(const std::string& id, const Status& status) {
    const std::string d = dir(id);
    if (exists(d + "/vocals.wav") && exists(d + "/no_vocals.wav")) return;
    ensureModel(status);
    status("보컬 분리 중... (노래 길이의 1~2배쯤 걸립니다)");
    // ycs_separate 는 exe 옆에 있다 (개발 중에는 build/Release, 설치판은 같은 폴더)
    std::string tool = paths::exeDir() + "/ycs_separate";
#ifdef _WIN32
    tool += ".exe";
#endif
    if (!exists(tool)) throw std::runtime_error("보컬 분리 도구가 없습니다: " + tool);
    std::string cmd = "\"" + tool + "\" \"" + modelPath() + "\" \"" + d + "/mix.wav\" \"" + d + "\"";
    int rc = paths::runCommand(cmd, logPath());
    if (rc != 0 || !exists(d + "/vocals.wav")) throw std::runtime_error("보컬 분리 실패 (exit code " + std::to_string(rc) + "). 로그: " + logPath());
}

int separationProgress() {
    std::ifstream in(fs::u8path(logPath()), std::ios::binary);
    if (!in) return -1;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = std::max<std::streamoff>(0, size - 4096);
    in.seekg(from);
    std::string tail((size_t)(size - from), '\0');
    in.read(&tail[0], tail.size());
    size_t p = tail.rfind("progress ");
    if (p == std::string::npos) return -1;
    // 마지막 "progress N" 뒤에 "done" 이 있으면 끝난 것
    if (tail.find("done ", p) != std::string::npos) return 100;
    return std::atoi(tail.c_str() + p + 9);
}

std::vector<pitch::Frame> analyzeMelody(const std::string& id, const Status& status) {
    const std::string d = dir(id);
    status("보컬 음정 선 뽑는 중...");
    auto pcm = audio::loadWav(d + "/vocals.wav", pitch::kRate);
    auto frames = pitch::Tracker::analyzeAll(pcm);
    // 분리된 보컬에는 반주가 조금 새어 들어온다 (특히 전주의 신디사이저 멜로디). 노래 소리의 큰 쪽(90 퍼센타일) 보다
    // 16 dB 넘게 작은 프레임은 멜로디로 치지 않는다. 노래 음역 밖(80 Hz 미만) 도 뺀다
    std::vector<float> dbs;
    for (const auto& f : frames) if (f.voiced) dbs.push_back(f.db);
    if (!dbs.empty()) {
        std::sort(dbs.begin(), dbs.end());
        const float thr = dbs[dbs.size() * 9 / 10] - 16.f;
        for (auto& f : frames) if (f.voiced && (f.db < thr || f.hz < 80.f)) { f.voiced = false; f.hz = 0.f; }
    }
    // 옥타브 튐 바로잡기: 앞뒤 ±10 프레임의 중앙값과 두 배/절반 차이면 옮기고, 그래도 1.5 배 넘게 다르면 뺀다. 그 뒤 7 프레임 중앙값으로 매끈하게
    {
        std::vector<float> hz(frames.size());
        for (size_t i = 0; i < frames.size(); ++i) hz[i] = frames[i].voiced ? frames[i].hz : 0.f;
        std::vector<float> nbr;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (hz[i] <= 0.f) continue;
            nbr.clear();
            for (size_t j = i >= 10 ? i - 10 : 0; j < std::min(frames.size(), i + 11); ++j) if (j != i && hz[j] > 0.f) nbr.push_back(hz[j]);
            if (nbr.size() < 5) continue;
            std::nth_element(nbr.begin(), nbr.begin() + nbr.size() / 2, nbr.end());
            const float med = nbr[nbr.size() / 2], ratio = hz[i] / med;
            if (std::fabs(ratio - 2.f) < 0.25f) frames[i].hz = hz[i] * 0.5f;
            else if (std::fabs(ratio - 0.5f) < 0.07f) frames[i].hz = hz[i] * 2.f;
            else if (ratio > 1.5f || ratio < 0.67f) { frames[i].voiced = false; frames[i].hz = 0.f; }
        }
        std::vector<float> out(frames.size(), 0.f), win;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!frames[i].voiced) continue;
            win.clear();
            for (size_t j = i >= 3 ? i - 3 : 0; j < std::min(frames.size(), i + 4); ++j) if (frames[j].voiced) win.push_back(frames[j].hz);
            std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
            out[i] = win[win.size() / 2];
        }
        for (size_t i = 0; i < frames.size(); ++i) if (frames[i].voiced) frames[i].hz = out[i];
    }
    // 5 프레임(50 ms) 이하의 빈틈은 이어 주고, 15 프레임(150 ms) 미만의 섬은 뺀다
    for (size_t i = 0; i < frames.size();) {
        if (frames[i].voiced) { ++i; continue; }
        size_t j = i;
        while (j < frames.size() && !frames[j].voiced) ++j;
        if (i > 0 && j < frames.size() && j - i <= 5) {
            const float a = frames[i - 1].hz, b = frames[j].hz;
            for (size_t k = i; k < j; ++k) { frames[k].voiced = true; frames[k].hz = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1); }
        }
        i = j;
    }
    for (size_t i = 0; i < frames.size();) {
        if (!frames[i].voiced) { ++i; continue; }
        size_t j = i;
        while (j < frames.size() && frames[j].voiced) ++j;
        if (j - i < 15) for (size_t k = i; k < j; ++k) { frames[k].voiced = false; frames[k].hz = 0.f; }
        i = j;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : frames) arr.push_back({f.voiced ? f.hz : 0.f, f.clarity, f.db});
    nlohmann::json j = {{"v", kMelodyVersion}, {"frames", arr}};
    std::ofstream out(fs::u8path(d + "/melody.json"), std::ios::binary);
    out << j.dump();
    return frames;
}

std::vector<pitch::Frame> loadMelody(const std::string& id) {
    std::vector<pitch::Frame> frames;
    std::ifstream in(fs::u8path(dir(id) + "/melody.json"), std::ios::binary);
    if (!in) return frames;
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object() || j.value("v", 0) != kMelodyVersion) return frames;  // 옛 형식이면 다시 뽑는다
        for (const auto& e : j.at("frames")) {
            pitch::Frame f;
            f.hz = e.at(0).get<float>();
            f.clarity = e.at(1).get<float>();
            f.db = e.at(2).get<float>();
            f.voiced = f.hz > 0.f;
            frames.push_back(f);
        }
    } catch (...) {
        frames.clear();
    }
    return frames;
}

}  // namespace song
