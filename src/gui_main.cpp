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
    int voicedRun = 0;                // 이어진 유성음 프레임 수 (짧은 튐 거르기)
    std::vector<pitch::Frame> pendingFrames;
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
    float sensitivity = 60.f;         // 마이크 민감도 0~100 % (높을수록 거친 소리 · 작은 소리도 음으로 친다)
    void applySensitivity() {
        // 0 → 주기성 0.75 · 바닥 +18 dB (조용한 방, 또렷한 소리만), 1 → 0.40 · +8 dB (립트릴처럼 거친 소리까지)
        const float k = sensitivity / 100.f;
        const float clarity = 0.75f - 0.35f * k, above = 18.f - 10.f * k;
        tracker.setSensitivity(clarity, above);
        songTracker.setSensitivity(clarity, above);
    }
    double sumAbsCents = 0;
    int scoredFrames = 0, okFrames = 0;
    float windowSec = 12.f;           // 가로 12 초 (천천히 흐르게)
    float viewSpan = 16.f;            // 세로로 보이는 반음 수 (휠로 조절). 작을수록 건반 간격이 넓다
    float viewCenter = 55.f;          // 세로 가운데 음. 연습을 시작하면 목표 범위에 맞춰 한 번 정한다
    bool autoRange = false;           // 켜면 음이 화면 밖으로 나갈 때 옮긴다 (기본은 가만히)
    int manualCenter = 55;

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
    float songSpan = 14.f;            // 노래 탭 세로 반음 수
    float songCenter = 60.f;          // 지금 보이는 멜로디의 가운데를 따라간다
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
            tracker.setWindowMs(40);  // 립트릴의 입술 떨림(약 25~30 Hz) 보다 긴 창으로 봐야 음이 끊기지 않는다
            applySensitivity();
        } catch (const std::exception& e) {
            micError = e.what();
            micOn = false;
        }
    }
    void stopMic() { mic.stop(); micOn = false; haveCurrent = false; }

    void startDrill() {
        running = true;
        // 세로 범위를 목표에 맞춰 한 번 정한다 (연습 중에는 움직이지 않는다)
        if (drill == Drill::Siren) { viewCenter = (lowMidi + highMidi) / 2.f; viewSpan = std::clamp((float)(highMidi - lowMidi) + 6.f, 10.f, 60.f); }
        else if (drill == Drill::Scale5) { viewCenter = scaleStartMidi + 3.5f + (highMidi - lowMidi) / 2.f; viewSpan = std::clamp((float)(highMidi - lowMidi) + 13.f, 12.f, 60.f); }
        else if (drill == Drill::Sustain) { viewCenter = (float)sustainMidi; viewSpan = std::max(viewSpan, 10.f); }
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
            // 잠깐 튄 소리(2 프레임 이하) 는 그리지 않는다 — 방 소음 · 숨소리가 점으로 깜빡이는 걸 막는다
            if (f.voiced) ++voicedRun; else voicedRun = 0;
            if (f.voiced && voicedRun < 3) { pendingFrames.push_back(f); f.voiced = false; f.hz = 0.f; }
            else if (f.voiced && voicedRun == 3) {
                // 3 프레임째에 앞의 두 프레임도 살린다
                const size_t n = hist.size();
                for (size_t k = 0; k < pendingFrames.size() && k < n; ++k) hist[n - pendingFrames.size() + k] = pendingFrames[k];
                pendingFrames.clear();
            } else pendingFrames.clear();
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
        if (hist.empty()) return;  // 탭을 막 바꿔 프레임이 아직 없을 때 (여기서 back() 을 읽으면 죽는다)
        const auto& last = hist.back();
        if (last.voiced) {
            const float m = pitch::hzToMidi(last.hz);
            smoothMidi = haveCurrent ? smoothMidi + (m - smoothMidi) * 0.5f : m;
            haveCurrent = true;
            if (autoRange && (m < viewCenter - viewSpan / 2 + 1.f || m > viewCenter + viewSpan / 2 - 1.f)) viewCenter = std::round(m);  // 화면 밖으로 나갈 때만 옮긴다
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

    // 그래프 위에서 휠: 보이는 반음 수 (6 ~ 36). Shift+휠 또는 왼쪽 드래그: 위아래로 이동
    void zoomWithWheel(ImVec2 p, ImVec2 size, float* span, float* center) {
        const ImVec2 m = ImGui::GetMousePos();
        if (m.x < p.x || m.x > p.x + size.x || m.y < p.y || m.y > p.y + size.y) return;
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.f) {
            if (io.KeyShift) *center = std::clamp(*center + io.MouseWheel, 24.f, 96.f);
            else *span = std::clamp(*span - io.MouseWheel * 2.f, 6.f, 60.f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.f)) {
            const float dy = io.MouseDelta.y;
            *center = std::clamp(*center + dy / size.y * *span, 24.f, 96.f);
        }
    }

    // 건반 눈금 (두 탭 공용). 흰 건반은 밝은 띠, 검은 건반은 어두운 띠, 칸이 넓으면 음 이름을 모두 적는다
    void drawKeyboardGrid(ImDrawList* dl, ImVec2 p, ImVec2 size, int lowM, int highM, float labelW, const std::function<float(float)>& yOf) {
        ImFont* font = ImGui::GetFont();
        const ImVec2 g0(p.x + labelW, p.y), g1(p.x + size.x, p.y + size.y);
        for (int m = lowM; m <= highM; ++m) {
            const int n = ((m % 12) + 12) % 12;
            const bool black = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
            const float y0 = yOf(m + 0.5f), y1 = yOf(m - 0.5f);
            if (!black) dl->AddRectFilled(ImVec2(g0.x, y0), ImVec2(g1.x, y1), IM_COL32(255, 255, 255, 7));
            if (n == 0 || n == 5) dl->AddLine(ImVec2(g0.x, y1), ImVec2(g1.x, y1), IM_COL32(255, 255, 255, n == 0 ? 55 : 20));  // C 와 F 자리에만 선
            const float rowH = y1 - y0;
            if (!black && rowH >= 11.f * uiScale) {
                const std::string name = pitch::noteName(m);
                const float fs = 13.f * uiScale;
                dl->AddText(font, fs, ImVec2(p.x + 4, (y0 + y1) / 2 - fs / 2), IM_COL32(200, 200, 200, n == 0 ? 255 : 130), name.c_str());
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
        // 목표가 있고 소리를 안 낼 땐 목표 쪽으로, 아무것도 없으면 그대로
        if (autoRange && running) {
            float tg;
            if (targetAt((frameNo - drillStartFrame) * 0.01, &tg) && (tg < viewCenter - viewSpan / 2 + 1.f || tg > viewCenter + viewSpan / 2 - 1.f)) viewCenter = std::round(tg);
        }
        if (!autoRange) viewCenter = (float)manualCenter;
        const float lo = viewCenter - viewSpan / 2, hi = viewCenter + viewSpan / 2;
        auto yOf = [&](float midi) { return g1.y - (midi - lo) / (hi - lo) * gh; };
        const float nowT = frameNo * 0.01f;
        auto xOf = [&](double t) { return (float)(g1.x - (nowT - t) / windowSec * gw); };
        drawKeyboardGrid(dl, p, size, (int)std::floor(lo), (int)std::ceil(hi), labelW, yOf);
        zoomWithWheel(p, size, &viewSpan, &viewCenter);
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
                if (!f.voiced) {
                    // 4 프레임(40 ms) 이하의 빈틈은 그냥 잇는다 (립트릴은 입술이 닫힐 때마다 잠깐 끊긴다)
                    size_t j = i;
                    while (j < hist.size() && !hist[j].voiced && j - i <= 4) ++j;
                    if (j < hist.size() && hist[j].voiced && j - i <= 4 && !pts.empty()) continue;
                    flush();
                    continue;
                }
                // 앞뒤 프레임과 중앙값 (떨림을 줄여 선이 차분하게)
                float m = pitch::hzToMidi(f.hz);
                if (i > 0 && i + 1 < hist.size() && hist[i - 1].voiced && hist[i + 1].voiced) {
                    float a = pitch::hzToMidi(hist[i - 1].hz), b = pitch::hzToMidi(hist[i + 1].hz);
                    m = std::max(std::min(a, b), std::min(std::max(a, b), m));
                }
                // 바로 앞 점과 7 반음 넘게 차이 나면 (튄 값) 선을 잇지 않는다
                if (!pts.empty() && std::fabs(yOf(m) - pts.back().y) > 7.f * gh / (hi - lo)) flush();
                pts.push_back(ImVec2(xOf(t), yOf(m)));
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
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(140 * uiScale);
        if (ImGui::SliderFloat("##sens", &sensitivity, 0.f, 100.f, "민감도 %.0f%%")) applySensitivity();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("립트릴이 잘 안 잡히면 올리고, 가만히 있는데 점이 찍히면 내리세요");

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
        ImGui::Checkbox("화면 밖이면 옮기기", &autoRange);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("연습을 시작하면 세로 범위를 목표 음에 맞춰 한 번 정하고 가만히 둡니다.\n켜면 내 음이 화면 밖으로 나갈 때만 옮깁니다. 휠: 확대 · 축소, Shift+휠 · 드래그: 위아래 이동");
        if (!autoRange) {
            ImGui::SameLine();
            if (manualCenter == 55 && viewCenter != 55.f) manualCenter = (int)std::lround(viewCenter);
            noteSlider("가운데 음", &manualCenter, 30, 90, 140 * uiScale);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * uiScale);
        ImGui::SliderFloat("##span", &viewSpan, 6.f, 60.f, "%.0f 반음 보기");
        ImGui::TextDisabled("%s", kDrillHelp[(int)drill]);
        if (micOn && !hist.empty()) {
            // 진단: 왜 안 찍히는지 볼 수 있게 (입력 크기 · 바닥 소음 · 주기성). 바닥 + 문턱보다 작거나 주기성이 낮으면 안 찍힌다
            const auto& f = hist.back();
            const float needDb = f.noiseDb + (18.f - 10.f * sensitivity / 100.f), needCl = 0.75f - 0.35f * sensitivity / 100.f;
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("입력 %.0f dB (필요 %.0f) · 주기성 %.2f (필요 %.2f)%s", f.db, needDb, f.clarity, needCl,
                                f.voiced ? "" : f.db <= needDb ? "  ← 소리가 작아요 (민감도를 올리거나 마이크 가까이)" : f.clarity < needCl ? "  ← 음이 또렷하지 않아요 (민감도를 올리세요)" : "");
        }

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
        // 세로 가운데: 멜로디 전체의 중앙값에서 시작 (그 뒤로는 보이는 구간을 따라간다)
        std::vector<float> ms;
        for (const auto& f : melody) if (f.voiced) ms.push_back(pitch::hzToMidi(f.hz));
        if (ms.size() > 100) {
            std::sort(ms.begin(), ms.end());
            const float lo2 = ms[ms.size() / 50], hi2 = ms[ms.size() * 49 / 50];  // 2 ~ 98 퍼센타일
            songCenter = std::round((lo2 + hi2) / 2);
            songSpan = std::clamp(std::ceil(hi2 - lo2) + 4.f, 10.f, 30.f);
        }
        if (startAtSec > 0) { engine.seek(startAtSec); startAtSec = 0; }
        songTracker.reset();
        songTracker.setWindowMs(40);
        applySensitivity();
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
        const double now = engine.positionSec();
        const double tLeft = now - songWindowSec * 0.35, tRight = now + songWindowSec * 0.65;
        // 세로 축은 노래를 불러올 때 멜로디 범위에 맞춰 고정한다 (재생 중에 움직이면 어지럽다). 휠로 확대, Shift+휠 · 드래그로 이동
        const float lo = songCenter - songSpan / 2, hi = songCenter + songSpan / 2;
        auto yOf = [&](float midi) { return g1.y - (midi - lo) / (hi - lo) * gh; };
        auto xOf = [&](double t) { return (float)(g0.x + (t - tLeft) / songWindowSec * gw); };
        drawKeyboardGrid(dl, p, size, (int)std::floor(lo), (int)std::ceil(hi), labelW, yOf);
        zoomWithWheel(p, size, &songSpan, &songCenter);
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
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y) < 4.f) {
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
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(120 * uiScale);
        ImGui::SliderFloat("##sspan", &songSpan, 6.f, 60.f, "%.0f 반음 보기");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("세로로 보이는 음의 폭. 그래프 위에서 마우스 휠로도 바꿉니다. 멜로디가 더 넓게 움직이면 자동으로 조금 넓어집니다");

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
            hist.clear();
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
