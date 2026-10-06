// YouCanSingWell GUI: 실시간 음정 그래프 + 립트릴 · 음계 연습 + 노래 따라 부르기
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#else
#include <OpenGL/gl3.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "paths.h"
#include "pitch.h"
#include "song.h"

namespace fs = std::filesystem;

namespace {

void glfwError(int code, const char* desc) { fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

void fatalBox(const std::string& msg) {
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "YouCanSingWell", MB_OK | MB_ICONERROR);
#else
    fprintf(stderr, "%s\n", msg.c_str());
#endif
}

std::string fmtTime(double sec) {
    const int s = (int)sec;
    char buf[16];
    snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// 연습 종류
enum class Drill { Free = 0, Siren, Scale5, Sustain };
const char* kDrillNames[] = {"자유 (음정만 보기)", "사이렌 (립트릴)", "5음 음계", "지속음"};
const char* kDrillHelp[] = {
    "그냥 소리를 내 보세요. 지금 음과 센트 차이가 위에 나옵니다.",
    "입술을 털며 '브르르' 소리로 파란 선을 따라 천천히 올라갔다 내려오세요. 소리가 끊기지 않게 숨을 고르게.",
    "도레미파솔파미레도 — 한 번 부를 때마다 반음씩 올라갑니다. 립트릴이나 '우' 로 해 보세요.",
    "파란 선의 음을 길게 붙드세요. 노란 선이 흔들리지 않게.",
};

constexpr int kHistoryFrames = 1500;  // 15 초

ImU32 centsColor(float cents) {
    const float a = std::fabs(cents);
    if (a <= 25.f) return IM_COL32(120, 230, 120, 255);
    if (a <= 50.f) return IM_COL32(255, 200, 80, 255);
    return IM_COL32(255, 110, 110, 255);
}

// 노래 가져오기 (백그라운드 스레드)
struct SongLoader {
    std::thread th;
    std::mutex m;
    bool busy = false, done = false, ok = false;
    std::string status, err, id;
    bool separating = false;
    // 결과
    std::vector<pitch::Frame> melody;
    std::vector<float> inst, vocal;   // 48 kHz mono
    song::Info info;

    void start(const std::string& videoId) {
        if (th.joinable()) th.join();
        { std::lock_guard<std::mutex> lock(m); busy = true; done = false; ok = false; err.clear(); id = videoId; status = "준비 중..."; separating = false; }
        th = std::thread([this, videoId] {
            auto setStatus = [this](const std::string& s) { std::lock_guard<std::mutex> lock(m); status = s; separating = s.rfind("보컬 분리 중", 0) == 0; };
            std::string e;
            try {
                song::download(videoId, setStatus);
                song::separate(videoId, setStatus);
                auto mel = song::loadMelody(videoId);
                if (mel.empty()) mel = song::analyzeMelody(videoId, setStatus);
                setStatus("반주 · 보컬 불러오는 중...");
                auto i = audio::loadWav(song::dir(videoId) + "/no_vocals.wav", kSampleRate);
                auto v = audio::loadWav(song::dir(videoId) + "/vocals.wav", kSampleRate);
                std::lock_guard<std::mutex> lock(m);
                melody = std::move(mel);
                inst = std::move(i);
                vocal = std::move(v);
                info = song::info(videoId);
            } catch (const std::exception& ex) {
                e = ex.what();
            }
            std::lock_guard<std::mutex> lock(m);
            busy = false;
            done = true;
            ok = e.empty();
            err = e;
            separating = false;
        });
    }
    ~SongLoader() { if (th.joinable()) th.join(); }
};

struct App {
    GLFWwindow* window = nullptr;
    float uiScale = 1.f;
    int tab = 0;  // 0 연습, 1 노래
    int forceTab = -1;  // 다음 프레임에 이 탭을 선택 (노래를 가져오면 노래 탭으로)

    // ---- 공통: 마이크 · 음정 ----
    LiveInput mic;
    ToneGenerator tone;
    pitch::Tracker tracker;
    std::string micError;
    bool micOn = false;

    std::deque<pitch::Frame> hist;    // 연습 탭: 10 ms 프레임 이력
    long long frameNo = 0;
    float smoothMidi = 0.f;
    bool haveCurrent = false;

    // ---- 연습 ----
    Drill drill = Drill::Siren;
    bool running = false;
    long long drillStartFrame = 0;
    int lowMidi = 48, highMidi = 60;
    float sirenPeriodSec = 6.f;
    float scaleNoteSec = 0.45f;
    int scaleStartMidi = 48;
    int sustainMidi = 55;
    bool guideTone = true;
    float guideVolume = 0.25f;
    double sumAbsCents = 0;
    int scoredFrames = 0, okFrames = 0;
    float windowSec = 8.f;
    int viewLow = 43, viewHigh = 67;
    bool autoRange = true;

    // ---- 노래 ----
    SongEngine engine;
    SongLoader loader;
    pitch::Tracker songTracker;
    char urlBuf[512] = "";
    std::string songMessage;
    std::vector<song::Info> libraryList;
    bool libraryDirty = true;
    bool songLoaded = false;
    song::Info songInfo;
    std::vector<pitch::Frame> melody;          // 10 ms, 노래 시각 기준
    struct Sung { double t; float midi; bool voiced; };
    std::deque<Sung> sung;                     // 내가 부른 음정 (노래 시각)
    float gInst = 80.f, gVocal = 30.f, gMon = 70.f;   // 퍼센트
    double startAtSec = 0;                     // --at <초> 로 시작 위치 지정 (테스트용)
    bool octaveFree = true;                    // 옥타브 차이는 무시 (남녀 음역)
    bool pitchShiftLine = false;
    int songViewLow = 48, songViewHigh = 76;
    double songSumAbs = 0;
    int songScored = 0, songOk = 0;
    float songWindowSec = 10.f;                // 그래프 가로 범위 (지금이 왼쪽 35 % 지점)
    double lastSongPos = 0;

    // ======== 연습 탭 ========
    bool targetAt(double t, float* midi) const {
        switch (drill) {
            case Drill::Free: return false;
            case Drill::Siren: {
                const double ph = std::fmod(t / sirenPeriodSec, 1.0);
                const double tri = ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0;
                *midi = (float)(lowMidi + (highMidi - lowMidi) * tri);
                return true;
            }
            case Drill::Scale5: {
                static const int steps[] = {0, 2, 4, 5, 7, 5, 4, 2, 0};
                const double rep = scaleNoteSec * 9 + 0.8;
                const int cycle = (int)(t / rep);
                const double u = t - cycle * rep;
                if (u >= scaleNoteSec * 9) return false;
                const int span = std::max(1, highMidi - lowMidi);
                const int key = scaleStartMidi + (cycle % (span + 1));
                *midi = (float)(key + steps[(int)(u / scaleNoteSec)]);
                return true;
            }
            case Drill::Sustain: *midi = (float)sustainMidi; return true;
        }
        return false;
    }

    void startMic() {
        try {
            mic.start();
            micOn = true;
            micError.clear();
            tracker.reset();
        } catch (const std::exception& e) {
            micError = e.what();
            micOn = false;
        }
    }
    void stopMic() { mic.stop(); micOn = false; haveCurrent = false; }

    void startDrill() {
        running = true;
        drillStartFrame = frameNo;
        sumAbsCents = 0;
        scoredFrames = okFrames = 0;
        if (guideTone) { try { tone.start(); } catch (const std::exception& e) { micError = e.what(); } }
    }
    void stopDrill() { running = false; tone.set(0.f, 0.f); }

    void updatePractice() {
        if (!micOn) return;
        auto pcm = mic.drain();
        if (pcm.empty()) return;
        auto frames = tracker.push(pcm.data(), pcm.size());
        for (auto& f : frames) {
            hist.push_back(f);
            if ((int)hist.size() > kHistoryFrames) hist.pop_front();
            float target = 0.f;
            const double t = (frameNo - drillStartFrame) * 0.01;
            if (running && f.voiced && targetAt(t, &target)) {
                const float cents = (pitch::hzToMidi(f.hz) - target) * 100.f;
                sumAbsCents += std::fabs(cents);
                ++scoredFrames;
                if (std::fabs(cents) <= 25.f) ++okFrames;
            }
            ++frameNo;
        }
        const auto& last = hist.back();
        if (last.voiced) {
            const float m = pitch::hzToMidi(last.hz);
            smoothMidi = haveCurrent ? smoothMidi + (m - smoothMidi) * 0.5f : m;
            haveCurrent = true;
            if (autoRange) {
                if (m < viewLow + 2) viewLow = (int)std::floor(m) - 3;
                if (m > viewHigh - 2) viewHigh = (int)std::ceil(m) + 3;
                if (viewHigh - viewLow < 12) viewHigh = viewLow + 12;
            }
        } else {
            int quiet = 0;
            for (auto it = hist.rbegin(); it != hist.rend() && !it->voiced && quiet <= 30; ++it) ++quiet;
            if (quiet > 30) haveCurrent = false;
        }
        if (running && guideTone) {
            float target = 0.f;
            const double t = (frameNo - drillStartFrame) * 0.01;
            if (targetAt(t, &target)) tone.set(pitch::midiToHz(target), guideVolume);
            else tone.set(0.f, 0.f);
        } else {
            tone.set(0.f, 0.f);
        }
    }

    // 건반 눈금 (두 탭 공용). 반환: 그래프 영역 좌상단 x (이름 칸 다음)
    void drawKeyboardGrid(ImDrawList* dl, ImVec2 p, ImVec2 size, int lowM, int highM, float labelW, const std::function<float(float)>& yOf) {
        ImFont* font = ImGui::GetFont();
        const ImVec2 g0(p.x + labelW, p.y), g1(p.x + size.x, p.y + size.y);
        for (int m = lowM; m <= highM; ++m) {
            const int n = ((m % 12) + 12) % 12;
            const bool black = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
            const float y0 = yOf(m + 0.5f), y1 = yOf(m - 0.5f);
            if (!black) dl->AddRectFilled(ImVec2(g0.x, y0), ImVec2(g1.x, y1), IM_COL32(255, 255, 255, 10));
            dl->AddLine(ImVec2(g0.x, y1), ImVec2(g1.x, y1), IM_COL32(255, 255, 255, n == 0 ? 60 : 22));
            if (!black && (y1 - y0) >= 11.f * uiScale) {
                const std::string name = pitch::noteName(m);
                dl->AddText(font, 12.f * uiScale, ImVec2(p.x + 4, (y0 + y1) / 2 - 6.f * uiScale), IM_COL32(200, 200, 200, n == 0 ? 255 : 140), name.c_str());
            }
        }
    }

    void drawPracticeGraph(ImVec2 size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(24, 24, 28, 255), 6.f);
        const float labelW = 44.f * uiScale;
        const ImVec2 g0(p.x + labelW, p.y), g1(p.x + size.x, p.y + size.y);
        const float gw = g1.x - g0.x, gh = g1.y - g0.y;
        const float lo = (float)viewLow - 0.5f, hi = (float)viewHigh + 0.5f;
        auto yOf = [&](float midi) { return g1.y - (midi - lo) / (hi - lo) * gh; };
        const float nowT = frameNo * 0.01f;
        auto xOf = [&](double t) { return (float)(g1.x - (nowT - t) / windowSec * gw); };
        drawKeyboardGrid(dl, p, size, viewLow, viewHigh, labelW, yOf);
        dl->PushClipRect(g0, g1, true);
        if (running && drill != Drill::Free) {
            std::vector<ImVec2> pts;
            const double startT = drillStartFrame * 0.01;
            for (float x = g0.x; x <= g1.x; x += 2.f) {
                const double t = nowT - (g1.x - x) / gw * windowSec;
                float midi;
                if (t >= startT && targetAt(t - startT, &midi)) pts.push_back(ImVec2(x, yOf(midi)));
                else { if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(90, 170, 255, 220), 3.f * uiScale); pts.clear(); }
            }
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(90, 170, 255, 220), 3.f * uiScale);
            float midi;
            if (targetAt(nowT - startT, &midi)) dl->AddCircleFilled(ImVec2(g1.x - 4, yOf(midi)), 5.f * uiScale, IM_COL32(90, 170, 255, 255));
        }
        {
            std::vector<ImVec2> pts;
            ImU32 col = IM_COL32(255, 210, 80, 255);
            const long long first = frameNo - (long long)hist.size();
            auto flush = [&] { if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); else if (pts.size() == 1) dl->AddCircleFilled(pts[0], 2.f * uiScale, col); pts.clear(); };
            for (size_t i = 0; i < hist.size(); ++i) {
                const auto& f = hist[i];
                const double t = (first + (long long)i) * 0.01;
                if (t < nowT - windowSec) continue;
                if (!f.voiced) { flush(); continue; }
                const float midi = pitch::hzToMidi(f.hz);
                ImU32 c = IM_COL32(255, 210, 80, 255);
                float target;
                if (running && t >= drillStartFrame * 0.01 && targetAt(t - drillStartFrame * 0.01, &target)) c = centsColor((midi - target) * 100.f);
                if (c != col && pts.size() >= 2) { dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); const ImVec2 keep = pts.back(); pts.clear(); pts.push_back(keep); }
                col = c;
                pts.push_back(ImVec2(xOf(t), yOf(midi)));
            }
            flush();
        }
        dl->PopClipRect();
        ImGui::Dummy(size);
    }

    void drawPracticeTab() {
        if (!micOn) {
            if (ImGui::Button("마이크 켜기")) startMic();
            if (!micError.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", micError.c_str()); }
        } else {
            if (ImGui::Button("마이크 끄기")) { stopDrill(); stopMic(); }
            ImGui::SameLine();
            ImGui::ProgressBar(std::min(1.f, mic.level() * 3.f), ImVec2(120 * uiScale, 0), "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("마이크 입력 크기");
        }
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(200 * uiScale);
        int d = (int)drill;
        if (ImGui::Combo("##drill", &d, kDrillNames, 4)) { drill = (Drill)d; if (running) startDrill(); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!micOn);
        if (!running) { if (ImGui::Button("시작 (Space)")) startDrill(); }
        else { if (ImGui::Button("멈춤 (Space)")) stopDrill(); }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("안내음", &guideTone)) { if (!guideTone) tone.set(0.f, 0.f); else if (running) { try { tone.start(); } catch (...) {} } }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * uiScale);
        ImGui::SliderFloat("##gv", &guideVolume, 0.f, 0.6f, "음량");

        auto noteSlider = [&](const char* label, int* midi, int lo, int hi, float width) {
            ImGui::SetNextItemWidth(width);
            const std::string fmt = pitch::noteName(*midi);
            ImGui::SliderInt(label, midi, lo, hi, fmt.c_str());
        };
        if (drill == Drill::Siren || drill == Drill::Scale5) {
            noteSlider("낮은 음", &lowMidi, 36, 72, 140 * uiScale);
            ImGui::SameLine();
            noteSlider("높은 음", &highMidi, 36, 84, 140 * uiScale);
            if (highMidi <= lowMidi) highMidi = lowMidi + 1;
            ImGui::SameLine();
            if (drill == Drill::Siren) { ImGui::SetNextItemWidth(140 * uiScale); ImGui::SliderFloat("한 번 오르내리는 시간", &sirenPeriodSec, 2.f, 12.f, "%.0f초"); }
            else {
                noteSlider("시작 키", &scaleStartMidi, 36, 72, 140 * uiScale);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140 * uiScale);
                ImGui::SliderFloat("한 음 길이", &scaleNoteSec, 0.25f, 1.0f, "%.2f초");
            }
        } else if (drill == Drill::Sustain) {
            noteSlider("목표 음", &sustainMidi, 36, 84, 200 * uiScale);
        }
        ImGui::SameLine(0, 16);
        ImGui::Checkbox("세로 범위 자동", &autoRange);
        if (!autoRange) {
            ImGui::SameLine();
            noteSlider("아래", &viewLow, 24, 84, 120 * uiScale);
            ImGui::SameLine();
            noteSlider("위", &viewHigh, 24, 96, 120 * uiScale);
            if (viewHigh - viewLow < 6) viewHigh = viewLow + 6;
        }
        ImGui::TextDisabled("%s", kDrillHelp[(int)drill]);

        if (haveCurrent) {
            const int nearest = (int)std::lround(smoothMidi);
            const float cents = (smoothMidi - nearest) * 100.f;
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "%s", pitch::noteName(nearest).c_str());
            ImGui::SetWindowFontScale(1.f);
            ImGui::SameLine(0, 12);
            ImGui::Text("%+d 센트  (%.1f Hz)", (int)std::lround(cents), pitch::midiToHz(smoothMidi));
            ImGui::SameLine(0, 16);
            drawCentsMeter(cents);
            if (running) {
                float target;
                if (targetAt((frameNo - drillStartFrame) * 0.01, &target)) {
                    const float diff = (smoothMidi - target) * 100.f;
                    ImGui::SameLine(0, 16);
                    ImGui::TextColored(ImColor(centsColor(diff)), "목표 %s  %s", pitch::noteName((int)std::lround(target)).c_str(),
                                       std::fabs(diff) <= 25 ? "좋아요" : diff > 0 ? "조금 내리세요 ↓" : "조금 올리세요 ↑");
                }
            }
        } else {
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextDisabled("—");
            ImGui::SetWindowFontScale(1.f);
            ImGui::SameLine(0, 12);
            ImGui::TextDisabled(micOn ? "소리를 내 보세요" : "마이크를 켜세요");
        }
        if (running && drill != Drill::Free && scoredFrames > 0) {
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("평균 오차 %.0f 센트 · ±25 센트 안 %d%%  (%.1f초)", sumAbsCents / scoredFrames, okFrames * 100 / scoredFrames, scoredFrames * 0.01);
        }
        ImVec2 avail = ImGui::GetContentRegionAvail();
        drawPracticeGraph(ImVec2(avail.x, std::max(120.f, avail.y - 4)));
    }

    void drawCentsMeter(float cents) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float mw = 200 * uiScale, mh = ImGui::GetTextLineHeight();
        dl->AddRectFilled(q, ImVec2(q.x + mw, q.y + mh), IM_COL32(40, 40, 46, 255), 3.f);
        dl->AddLine(ImVec2(q.x + mw / 2, q.y), ImVec2(q.x + mw / 2, q.y + mh), IM_COL32(255, 255, 255, 90));
        const float x = q.x + mw / 2 + std::clamp(cents, -50.f, 50.f) / 100.f * mw;
        dl->AddRectFilled(ImVec2(x - 3, q.y), ImVec2(x + 3, q.y + mh), centsColor(cents), 2.f);
        ImGui::Dummy(ImVec2(mw, mh));
    }

    // ======== 노래 탭 ========
    void requestSong(const std::string& input) {
        auto id = song::extractVideoId(input);
        if (!id) { songMessage = "유튜브 주소나 11자리 영상 ID 로 인식되지 않습니다"; return; }
        if (loader.busy) { songMessage = "다른 노래를 준비하는 중입니다"; return; }
        songMessage.clear();
        forceTab = 1;
        loader.start(*id);
    }

    void onSongLoaded() {
        std::lock_guard<std::mutex> lock(loader.m);
        melody = std::move(loader.melody);
        songInfo = loader.info;
        engine.load(std::move(loader.inst), std::move(loader.vocal));
        engine.setGains(gInst / 100.f, gVocal / 100.f, gMon / 100.f);
        sung.clear();
        songSumAbs = 0;
        songScored = songOk = 0;
        songLoaded = true;
        libraryDirty = true;
        // 세로 범위: 멜로디의 범위에 맞춘다
        std::vector<float> ms;
        for (const auto& f : melody) if (f.voiced) ms.push_back(pitch::hzToMidi(f.hz));
        if (ms.size() > 100) {
            std::sort(ms.begin(), ms.end());
            songViewLow = (int)std::floor(ms[ms.size() / 50]) - 2;        // 2 ~ 98 퍼센타일 (튀는 값은 무시)
            songViewHigh = (int)std::ceil(ms[ms.size() * 49 / 50]) + 2;
            if (songViewHigh - songViewLow < 12) songViewHigh = songViewLow + 12;
        }
        if (startAtSec > 0) { engine.seek(startAtSec); startAtSec = 0; }
        try { if (!engine.active()) { stopMic(); engine.start(); } } catch (const std::exception& e) { songMessage = e.what(); }
    }

    // 멜로디의 노래 시각 t 의 음 (없으면 false)
    bool melodyAt(double t, float* midi) const {
        const long long k = (long long)(t * 100.0);
        if (k < 0 || k >= (long long)melody.size() || !melody[k].voiced) return false;
        *midi = pitch::hzToMidi(melody[k].hz);
        return true;
    }

    // 옥타브 무시 비교: 내 음 − 멜로디 음 (반음), 옥타브 차이는 ±6 안으로 접는다
    float melodyDiff(float myMidi, float melMidi) const {
        float d = myMidi - melMidi;
        if (octaveFree) { d = std::fmod(d, 12.f); if (d > 6.f) d -= 12.f; if (d < -6.f) d += 12.f; }
        return d;
    }

    void updateSong() {
        // 로더 결과
        {
            std::lock_guard<std::mutex> lock(loader.m);
            if (loader.done) {
                loader.done = false;
                if (!loader.ok) songMessage = "실패: " + loader.err;
            } else goto afterLoad;
        }
        if (!loader.busy && loader.ok) onSongLoaded();
    afterLoad:
        if (!engine.active()) return;
        for (auto& ch : engine.drainMic()) {
            auto frames = songTracker.push(ch.pcm.data(), ch.pcm.size());
            // 이 덩어리의 프레임들은 덩어리 시작 시각부터 10 ms 간격
            for (size_t i = 0; i < frames.size(); ++i) {
                const double t = ch.songSec + i * 0.01;
                const bool play = engine.playing();
                Sung s{t, frames[i].voiced ? pitch::hzToMidi(frames[i].hz) : 0.f, frames[i].voiced};
                if (play) {
                    sung.push_back(s);
                    float mel;
                    if (s.voiced && melodyAt(t, &mel)) {
                        const float cents = melodyDiff(s.midi, mel) * 100.f;
                        songSumAbs += std::fabs(cents);
                        ++songScored;
                        if (std::fabs(cents) <= 50.f) ++songOk;
                    }
                }
                smoothMidi = s.voiced ? (haveCurrent ? smoothMidi + (s.midi - smoothMidi) * 0.5f : s.midi) : smoothMidi;
                if (s.voiced) haveCurrent = true;
            }
        }
        while (!sung.empty() && sung.front().t < engine.positionSec() - 60.0) sung.pop_front();
        // 되감기하면 그 뒤에 부른 기록은 지운다
        const double pos = engine.positionSec();
        if (pos < lastSongPos - 0.5) { while (!sung.empty() && sung.back().t > pos) sung.pop_back(); }
        lastSongPos = pos;
    }

    void drawSongGraph(ImVec2 size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(24, 24, 28, 255), 6.f);
        const float labelW = 44.f * uiScale;
        const ImVec2 g0(p.x + labelW, p.y), g1(p.x + size.x, p.y + size.y);
        const float gw = g1.x - g0.x, gh = g1.y - g0.y;
        const float lo = (float)songViewLow - 0.5f, hi = (float)songViewHigh + 0.5f;
        auto yOf = [&](float midi) { return g1.y - (midi - lo) / (hi - lo) * gh; };
        const double now = engine.positionSec();
        const double tLeft = now - songWindowSec * 0.35, tRight = now + songWindowSec * 0.65;
        auto xOf = [&](double t) { return (float)(g0.x + (t - tLeft) / songWindowSec * gw); };
        drawKeyboardGrid(dl, p, size, songViewLow, songViewHigh, labelW, yOf);
        dl->PushClipRect(g0, g1, true);
        // 멜로디 (파랑). 옥타브 무시면 내 음을 멜로디 옥타브로 옮겨 그리므로 멜로디는 그대로
        {
            std::vector<ImVec2> pts;
            const long long k0 = std::max(0LL, (long long)(tLeft * 100)), k1 = std::min((long long)melody.size(), (long long)(tRight * 100) + 1);
            auto flush = [&] { if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(90, 170, 255, 230), 3.f * uiScale); else if (pts.size() == 1) dl->AddCircleFilled(pts[0], 2.f * uiScale, IM_COL32(90, 170, 255, 230)); pts.clear(); };
            for (long long k = k0; k < k1; ++k) {
                if (!melody[k].voiced) { flush(); continue; }
                pts.push_back(ImVec2(xOf(k * 0.01), yOf(pitch::hzToMidi(melody[k].hz))));
            }
            flush();
        }
        // 내 음정 (멜로디와의 차이 색). 옥타브 무시면 멜로디 가까운 옥타브로 옮겨 그린다
        {
            std::vector<ImVec2> pts;
            ImU32 col = IM_COL32(255, 210, 80, 255);
            auto flush = [&] { if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); else if (pts.size() == 1) dl->AddCircleFilled(pts[0], 2.f * uiScale, col); pts.clear(); };
            for (const auto& s : sung) {
                if (s.t < tLeft) continue;
                if (s.t > tRight) break;
                if (!s.voiced) { flush(); continue; }
                float mel, midi = s.midi;
                ImU32 c = IM_COL32(255, 210, 80, 255);
                if (melodyAt(s.t, &mel)) {
                    const float d = melodyDiff(s.midi, mel);
                    c = centsColor(d * 100.f);
                    if (octaveFree) midi = mel + d;
                }
                if (c != col && pts.size() >= 2) { dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); const ImVec2 keep = pts.back(); pts.clear(); pts.push_back(keep); }
                col = c;
                pts.push_back(ImVec2(xOf(s.t), yOf(midi)));
            }
            flush();
        }
        // 지금 선
        const float xn = xOf(now);
        dl->AddLine(ImVec2(xn, g0.y), ImVec2(xn, g1.y), IM_COL32(255, 255, 255, 120), 2.f);
        dl->PopClipRect();
        ImGui::Dummy(size);
        // 클릭하면 그 시각으로
        if (ImGui::IsItemClicked()) {
            const float mx = ImGui::GetMousePos().x;
            if (mx >= g0.x) engine.seek(tLeft + (mx - g0.x) / gw * songWindowSec);
        }
    }

    void drawSongTab() {
        // 가져오기
        ImGui::SetNextItemWidth(420 * uiScale);
        const bool enter = ImGui::InputTextWithHint("##url", "유튜브 주소 또는 영상 ID", urlBuf, sizeof urlBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(loader.busy);
        if (ImGui::Button("가져오기") || enter) requestSong(urlBuf);
        ImGui::EndDisabled();
        if (loader.busy) {
            std::string st; bool sep;
            { std::lock_guard<std::mutex> lock(loader.m); st = loader.status; sep = loader.separating; }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", st.c_str());
            if (sep) {
                const int prog = song::separationProgress();
                ImGui::SameLine();
                ImGui::ProgressBar(prog < 0 ? 0.f : prog / 100.f, ImVec2(200 * uiScale, 0));
            }
        } else if (!songMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.6f, 0.6f, 1), "%s", songMessage.c_str());
        }
        // 받아 둔 노래
        if (libraryDirty) { libraryList = song::library(); libraryDirty = false; }
        if (!libraryList.empty()) {
            ImGui::SameLine(0, 16);
            ImGui::SetNextItemWidth(320 * uiScale);
            if (ImGui::BeginCombo("##lib", "받아 둔 노래 열기")) {
                for (const auto& it : libraryList) {
                    std::string label = it.title + (it.melody ? "" : "  (준비 안 됨)");
                    if (ImGui::Selectable(label.c_str())) { snprintf(urlBuf, sizeof urlBuf, "%s", it.id.c_str()); requestSong(it.id); }
                }
                ImGui::EndCombo();
            }
        }
        if (!songLoaded) {
            ImGui::TextDisabled("유튜브 노래를 가져오면 PC 안에서 보컬을 분리해 멜로디 선을 뽑습니다 (처음엔 모델 80MB 를 한 번 받고, 노래 길이의 1~2배쯤 걸립니다).");
            ImGui::TextDisabled("준비되면 반주와 함께 멜로디 선이 흐르고, 따라 부르면 내 음정이 겹쳐 보입니다. 이어폰을 쓰면 내 목소리도 함께 들립니다.");
            return;
        }
        // 재생 조작
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "%s", songInfo.title.c_str());
        if (engine.playing()) { if (ImGui::Button("일시정지 (Space)")) engine.pause(); }
        else { if (ImGui::Button("재생 (Space)")) engine.play(); }
        ImGui::SameLine();
        if (ImGui::Button("처음으로")) { engine.seek(0); sung.clear(); songSumAbs = 0; songScored = songOk = 0; }
        ImGui::SameLine();
        float pos = (float)engine.positionSec();
        const float dur = (float)engine.durationSec();
        ImGui::SetNextItemWidth(300 * uiScale);
        if (ImGui::SliderFloat("##pos", &pos, 0.f, dur, (fmtTime(pos) + " / " + fmtTime(dur)).c_str())) engine.seek(pos);
        ImGui::SameLine(0, 16);
        bool changed = false;
        ImGui::SetNextItemWidth(110 * uiScale); changed |= ImGui::SliderFloat("반주", &gInst, 0.f, 100.f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * uiScale); changed |= ImGui::SliderFloat("원곡 보컬", &gVocal, 0.f, 100.f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * uiScale); changed |= ImGui::SliderFloat("내 목소리", &gMon, 0.f, 150.f, "%.0f%%");
        if (changed) engine.setGains(gInst / 100.f, gVocal / 100.f, gMon / 100.f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("이어폰으로 들을 때 내 목소리 크기. 스피커로 들으면 울릴 수 있으니 0 으로");
        ImGui::SameLine(0, 16);
        ImGui::Checkbox("옥타브 무시", &octaveFree);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("원곡 가수와 음역이 달라 한 옥타브 위아래로 부를 때 맞는 것으로 칩니다");
        ImGui::SameLine(0, 16);
        ImGui::ProgressBar(std::min(1.f, engine.micLevel() * 3.f), ImVec2(100 * uiScale, 0), "");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("마이크 입력 크기");

        // 지금 음 · 점수
        if (haveCurrent) {
            const int nearest = (int)std::lround(smoothMidi);
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "%s", pitch::noteName(nearest).c_str());
            ImGui::SetWindowFontScale(1.f);
            float mel;
            if (melodyAt(engine.positionSec(), &mel)) {
                const float d = melodyDiff(smoothMidi, mel) * 100.f;
                ImGui::SameLine(0, 12);
                ImGui::TextColored(ImColor(centsColor(d)), "멜로디 %s  %s", pitch::noteName((int)std::lround(mel)).c_str(),
                                   std::fabs(d) <= 50 ? "좋아요" : d > 0 ? "조금 내리세요 ↓" : "조금 올리세요 ↑");
            }
        } else {
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextDisabled("—");
            ImGui::SetWindowFontScale(1.f);
        }
        if (songScored > 0) {
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("±50 센트 안 %d%% · 평균 오차 %.0f 센트  (부른 시간 %.0f초)", songOk * 100 / songScored, songSumAbs / songScored, songScored * 0.01);
        }
        ImVec2 avail = ImGui::GetContentRegionAvail();
        drawSongGraph(ImVec2(avail.x, std::max(120.f, avail.y - 4)));
    }

    void drawFrame() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ImGui::BeginTabBar("tabs")) {
            const int force = forceTab;
            forceTab = -1;
            if (ImGui::BeginTabItem("음정 연습", nullptr, force == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { if (tab != 0) switchTab(0); drawPracticeTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("노래 따라 부르기", nullptr, force == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { if (tab != 1) switchTab(1); drawSongTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // 탭을 바꾸면 마이크를 쓰는 쪽만 장치를 연다 (같은 마이크를 두 장치가 동시에 열지 않게)
    void switchTab(int t) {
        tab = t;
        haveCurrent = false;
        if (t == 0) {
            if (engine.active()) { engine.pause(); engine.stop(); }
            if (!micOn) startMic();
        } else {
            stopDrill();
            if (songLoaded && !engine.active()) { stopMic(); try { engine.start(); } catch (const std::exception& e) { songMessage = e.what(); } }
        }
    }

    void handleKeys() {
        if (ImGui::GetIO().WantTextInput) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (tab == 0) { if (micOn) { if (running) stopDrill(); else startDrill(); } }
            else if (songLoaded) { if (engine.playing()) engine.pause(); else engine.play(); }
        }
    }

    void update() {
        if (tab == 0) updatePractice();
        else updateSong();
        // 로더는 어느 탭에서든 끝날 수 있다
        if (tab == 0) {
            std::lock_guard<std::mutex> lock(loader.m);
            if (loader.done && !loader.ok) { loader.done = false; songMessage = "실패: " + loader.err; }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    paths::setup();
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1200, 760, "YouCanSingWell v" YCS_VERSION, nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FrameRounding = 4.0f;
    float scaleX = 1.0f, scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
#ifdef __APPLE__
    const float uiScale = 1.0f;
#else
    const float uiScale = std::max(1.0f, scaleX);
#endif
    ImGui::GetStyle().ScaleAllSizes(uiScale);
#ifdef _WIN32
    const char* fontPath = "C:/Windows/Fonts/malgun.ttf";
#else
    const char* fontPath = "/System/Library/Fonts/AppleSDGothicNeo.ttc";
#endif
    if (fs::exists(fontPath)) io.Fonts->AddFontFromFileTTF(fontPath, 18.0f * uiScale, nullptr, io.Fonts->GetGlyphRangesKorean());
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __APPLE__
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 130");
#endif

    App app;
    app.uiScale = uiScale;
    app.window = window;
    app.startMic();
    // youcansingwell [유튜브 주소] [--at 초]: 노래를 바로 가져오고 (테스트용) 그 위치에서 시작
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--at" && i + 1 < argc) app.startAtSec = atof(argv[++i]);
        else { snprintf(app.urlBuf, sizeof app.urlBuf, "%s", a.c_str()); app.requestSong(a); }
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.update();
        app.handleKeys();
        app.drawFrame();
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    app.stopDrill();
    app.tone.stop();
    app.mic.stop();
    app.engine.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
