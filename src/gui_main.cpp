// YouCanSingWell GUI: 실시간 음정 그래프 + 립트릴 · 음계 연습
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "audio.h"
#include "paths.h"
#include "pitch.h"

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

struct App {
    GLFWwindow* window = nullptr;
    float uiScale = 1.f;

    LiveInput mic;
    ToneGenerator tone;
    pitch::Tracker tracker;
    std::string micError;
    bool micOn = false;

    // 음정 이력 (10 ms 프레임). frameNo 는 마이크를 켠 뒤 몇 번째 프레임인지
    std::deque<pitch::Frame> hist;
    long long frameNo = 0;            // hist.back() 의 번호 + 1
    float smoothMidi = 0.f;           // 표시용으로 살짝 평활한 현재 음
    bool haveCurrent = false;

    // 연습
    Drill drill = Drill::Siren;
    bool running = false;
    long long drillStartFrame = 0;
    int lowMidi = 48, highMidi = 60;  // 사이렌 범위 C3 ~ C4
    float sirenPeriodSec = 6.f;       // 올라갔다 내려오는 데 걸리는 시간
    float scaleNoteSec = 0.45f;       // 5음 음계 한 음 길이
    int scaleStartMidi = 48;          // 5음 음계 시작 키
    int sustainMidi = 55;             // 지속음 목표 (G3)
    bool guideTone = true;
    float guideVolume = 0.25f;
    // 채점 (연습 시작 후 유성음 프레임 누적)
    double sumAbsCents = 0;
    int scoredFrames = 0, okFrames = 0;
    float lastSirenPhase = 0.f;

    // 그래프
    float windowSec = 8.f;
    int viewLow = 43, viewHigh = 67;  // 세로 범위 (G2 ~ G4)
    bool autoRange = true;

    // ---- 목표 음 (연습 시작 뒤 t 초) ----
    bool targetAt(double t, float* midi) const {
        switch (drill) {
            case Drill::Free: return false;
            case Drill::Siren: {
                const double ph = std::fmod(t / sirenPeriodSec, 1.0);          // 0~1
                const double tri = ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0;        // 0→1→0
                *midi = (float)(lowMidi + (highMidi - lowMidi) * tri);
                return true;
            }
            case Drill::Scale5: {
                static const int steps[] = {0, 2, 4, 5, 7, 5, 4, 2, 0};
                const double rep = scaleNoteSec * 9 + 0.8;                      // 한 번 부르고 0.8 초 쉼
                const int cycle = (int)(t / rep);
                const double u = t - cycle * rep;
                if (u >= scaleNoteSec * 9) return false;                        // 쉬는 구간
                const int span = std::max(1, highMidi - lowMidi);
                const int key = scaleStartMidi + (cycle % (span + 1));          // 반음씩 올리다 범위를 넘으면 처음부터
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

    void startDrill() {
        running = true;
        drillStartFrame = frameNo;
        sumAbsCents = 0;
        scoredFrames = okFrames = 0;
        if (guideTone) { try { tone.start(); } catch (const std::exception& e) { micError = e.what(); } }
    }
    void stopDrill() {
        running = false;
        tone.set(0.f, 0.f);
    }

    void update() {
        if (!micOn) return;
        auto pcm = mic.drain();
        if (pcm.empty()) return;
        auto frames = tracker.push(pcm.data(), pcm.size());
        for (auto& f : frames) {
            hist.push_back(f);
            if ((int)hist.size() > kHistoryFrames) hist.pop_front();
            // 채점: 목표가 있고 소리를 냈으면 센트 차이를 누적
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
        // 현재 음 (표시용, 살짝 평활)
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
            // 300 ms 넘게 조용하면 현재 음 표시를 지운다
            int quiet = 0;
            for (auto it = hist.rbegin(); it != hist.rend() && !it->voiced && quiet <= 30; ++it) ++quiet;
            if (quiet > 30) haveCurrent = false;
        }
        // 안내음
        if (running && guideTone) {
            float target = 0.f;
            const double t = (frameNo - drillStartFrame) * 0.01;
            if (targetAt(t, &target)) tone.set(pitch::midiToHz(target), guideVolume);
            else tone.set(0.f, 0.f);
        } else {
            tone.set(0.f, 0.f);
        }
    }

    // ---- 그리기 ----
    static ImU32 centsColor(float cents) {
        const float a = std::fabs(cents);
        if (a <= 25.f) return IM_COL32(120, 230, 120, 255);
        if (a <= 50.f) return IM_COL32(255, 200, 80, 255);
        return IM_COL32(255, 110, 110, 255);
    }

    void drawGraph(ImVec2 size) {
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

        // 건반 눈금: 흰 건반은 밝게, 검은 건반은 어둡게. C 마다 이름
        ImFont* font = ImGui::GetFont();
        for (int m = viewLow; m <= viewHigh; ++m) {
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
        dl->PushClipRect(g0, g1, true);
        // 목표 선 (파랑)
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
            // 지금 목표음 표시 (오른쪽 끝)
            float midi;
            if (targetAt(nowT - startT, &midi)) {
                dl->AddCircleFilled(ImVec2(g1.x - 4, yOf(midi)), 5.f * uiScale, IM_COL32(90, 170, 255, 255));
            }
        }
        // 내 음정 (노랑, 연습 중엔 차이 색)
        {
            std::vector<ImVec2> pts;
            ImU32 col = IM_COL32(255, 210, 80, 255);
            const long long first = frameNo - (long long)hist.size();
            for (size_t i = 0; i < hist.size(); ++i) {
                const auto& f = hist[i];
                const double t = (first + (long long)i) * 0.01;
                if (t < nowT - windowSec) continue;
                if (!f.voiced) { if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); else if (pts.size() == 1) dl->AddCircleFilled(pts[0], 2.f * uiScale, col); pts.clear(); continue; }
                const float midi = pitch::hzToMidi(f.hz);
                ImU32 c = IM_COL32(255, 210, 80, 255);
                float target;
                if (running && t >= drillStartFrame * 0.01 && targetAt(t - drillStartFrame * 0.01, &target)) c = centsColor((midi - target) * 100.f);
                if (c != col && pts.size() >= 2) { dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale); const ImVec2 keep = pts.back(); pts.clear(); pts.push_back(keep); }
                col = c;
                pts.push_back(ImVec2(xOf(t), yOf(midi)));
            }
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 2.5f * uiScale);
            else if (pts.size() == 1) dl->AddCircleFilled(pts[0], 2.f * uiScale, col);
        }
        dl->PopClipRect();
        ImGui::Dummy(size);
    }

    void drawFrame() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // 1행: 마이크 · 연습 선택 · 시작
        if (!micOn) {
            if (ImGui::Button("마이크 켜기")) startMic();
            if (!micError.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", micError.c_str()); }
        } else {
            if (ImGui::Button("마이크 끄기")) { stopDrill(); mic.stop(); micOn = false; haveCurrent = false; }
            ImGui::SameLine();
            // 입력 레벨
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

        // 2행: 연습 설정
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

        // 3행: 지금 음 · 센트 미터 · 점수
        if (haveCurrent) {
            const int nearest = (int)std::lround(smoothMidi);
            const float cents = (smoothMidi - nearest) * 100.f;
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "%s", pitch::noteName(nearest).c_str());
            ImGui::SetWindowFontScale(1.f);
            ImGui::SameLine(0, 12);
            ImGui::Text("%+d 센트  (%.1f Hz)", (int)std::lround(cents), pitch::midiToHz(smoothMidi));
            // 센트 미터: -50 ~ +50
            ImGui::SameLine(0, 16);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float mw = 200 * uiScale, mh = ImGui::GetTextLineHeight();
            dl->AddRectFilled(q, ImVec2(q.x + mw, q.y + mh), IM_COL32(40, 40, 46, 255), 3.f);
            dl->AddLine(ImVec2(q.x + mw / 2, q.y), ImVec2(q.x + mw / 2, q.y + mh), IM_COL32(255, 255, 255, 90));
            const float x = q.x + mw / 2 + std::clamp(cents, -50.f, 50.f) / 100.f * mw;
            dl->AddRectFilled(ImVec2(x - 3, q.y), ImVec2(x + 3, q.y + mh), centsColor(cents), 2.f);
            ImGui::Dummy(ImVec2(mw, mh));
            // 연습 중 목표와의 차이
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

        // 4행: 그래프
        ImVec2 avail = ImGui::GetContentRegionAvail();
        drawGraph(ImVec2(avail.x, std::max(120.f, avail.y - 4)));

        ImGui::End();
    }

    void handleKeys() {
        if (ImGui::GetIO().WantTextInput) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && micOn) { if (running) stopDrill(); else startDrill(); }
    }
};

}  // namespace

int main(int, char**) {
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
    GLFWwindow* window = glfwCreateWindow(1100, 700, "YouCanSingWell v" YCS_VERSION, nullptr, nullptr);
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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
