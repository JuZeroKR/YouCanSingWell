// 보컬 분리 도구 (demucs.cpp, Hybrid Transformer Demucs v4 4-source). 앱이 백그라운드로 실행한다.
//   ycs_separate <model.bin> <input.wav> <out dir> [threads]
// 결과: <out dir>/vocals.wav, <out dir>/no_vocals.wav (드럼 + 베이스 + 기타 합, 반주) — 둘 다 stereo 44.1 kHz.
// 진행률은 stdout 에 "progress <0~100>" 한 줄씩 찍는다.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <Eigen/Core>
#include <Eigen/Dense>

#include "dsp.hpp"
#include "model.hpp"
#include "tensor.hpp"

#include "miniaudio.h"

namespace fs = std::filesystem;

namespace {

constexpr int OVERLAP_SAMPLES = 44100 * 3;  // 조각 사이 겹침 3 초 (demucs.cpp 의 threaded_inference 와 같음)

// wav → 44.1 kHz 스테레오 (2 × N)
Eigen::MatrixXf loadAudio(const std::string& path) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, demucscpp::SUPPORTED_SAMPLE_RATE);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) throw std::runtime_error("오디오 파일을 열 수 없음: " + path);
    std::vector<float> inter;
    std::vector<float> chunk((size_t)demucscpp::SUPPORTED_SAMPLE_RATE * 2);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size() / 2, &got);
        inter.insert(inter.end(), chunk.begin(), chunk.begin() + (size_t)got * 2);
        if (r != MA_SUCCESS || got < chunk.size() / 2) break;
    }
    ma_decoder_uninit(&dec);
    const size_t n = inter.size() / 2;
    Eigen::MatrixXf m(2, (Eigen::Index)n);
    for (size_t i = 0; i < n; ++i) { m(0, (Eigen::Index)i) = inter[2 * i]; m(1, (Eigen::Index)i) = inter[2 * i + 1]; }
    return m;
}

void saveStereo(const std::string& path, const Eigen::Tensor3dXf& out, const std::vector<int>& targets) {
    const int n = (int)out.dimension(2);
    std::vector<float> inter((size_t)n * 2, 0.f);
    for (int t : targets)
        for (int i = 0; i < n; ++i) { inter[2 * i] += out(t, 0, i); inter[2 * i + 1] += out(t, 1, i); }
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, demucscpp::SUPPORTED_SAMPLE_RATE);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) throw std::runtime_error("파일을 저장할 수 없음: " + path);
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, inter.data(), (ma_uint64)n, &written);
    ma_encoder_uninit(&enc);
}

// 곡을 threads 조각으로 나눠 동시에 돌리고 겹침 구간은 삼각 가중치로 잇는다 (demucs.cpp cli-apps/threaded_inference.hpp 와 같은 방식)
Eigen::Tensor3dXf separate(const demucscpp::demucs_model& model, const Eigen::MatrixXf& audio, int threads) {
    const int total = (int)audio.cols();
    threads = std::max(1, std::min(threads, std::max(1, total / (44100 * 10))));  // 조각이 10 초보다 짧아지지 않게
    const int segLen = (int)std::ceil((float)total / threads);
    std::vector<Eigen::MatrixXf> segs;
    for (int i = 0; i < threads; ++i) {
        const int start = i * segLen, end = std::min(total, start + segLen);
        Eigen::MatrixXf seg = Eigen::MatrixXf::Zero(2, end - start + 2 * OVERLAP_SAMPLES);
        if (start > 0) seg.block(0, 0, 2, OVERLAP_SAMPLES) = audio.block(0, start - OVERLAP_SAMPLES, 2, OVERLAP_SAMPLES);
        else seg.block(0, 0, 2, OVERLAP_SAMPLES).colwise() = audio.col(0);
        if (end < total) {
            const int rem = std::min(OVERLAP_SAMPLES, total - end);
            seg.block(0, end - start + OVERLAP_SAMPLES, 2, rem) = audio.block(0, end, 2, rem);
        }
        seg.block(0, OVERLAP_SAMPLES, 2, end - start) = audio.block(0, start, 2, end - start);
        segs.push_back(seg);
    }
    std::vector<Eigen::Tensor3dXf> outs(threads);
    std::vector<float> progress(threads, 0.f);
    std::mutex m;
    auto report = [&] {
        float sum = 0.f;
        for (float p : progress) sum += p;
        printf("progress %d\n", (int)(sum / threads * 100.f));
        fflush(stdout);
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
        pool.emplace_back([&, i] {
            outs[i] = demucscpp::demucs_inference(model, segs[i], [&, i](float p, const std::string&) {
                std::lock_guard<std::mutex> lock(m);
                progress[i] = p;
                report();
            });
        });
    for (auto& t : pool) t.join();

    const int nb = model.is_4sources ? 4 : 6;
    Eigen::Tensor3dXf out(nb, 2, total);
    out.setZero();
    std::vector<float> weightSum(total, 0.f);
    for (int i = 0; i < threads; ++i) {
        const int start = i * segLen, end = std::min(total, start + segLen), len = end - start;
        for (int j = 0; j < len + 2 * OVERLAP_SAMPLES; ++j) {
            const int g = start - OVERLAP_SAMPLES + j;
            if (g < 0 || g >= total) continue;
            float w = 1.f;
            if (j < OVERLAP_SAMPLES) w = (float)(j + 1) / OVERLAP_SAMPLES;
            else if (j >= len + OVERLAP_SAMPLES) w = (float)(len + 2 * OVERLAP_SAMPLES - j) / OVERLAP_SAMPLES;
            for (int t = 0; t < nb; ++t)
                for (int ch = 0; ch < 2; ++ch) out(t, ch, g) += outs[i](t, ch, j) * w;
            weightSum[g] += w;
        }
    }
    for (int g = 0; g < total; ++g)
        if (weightSum[g] > 0)
            for (int t = 0; t < nb; ++t)
                for (int ch = 0; ch < 2; ++ch) out(t, ch, g) /= weightSum[g];
    return out;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 4) {
        fprintf(stderr, "usage: ycs_separate <model.bin> <input.wav> <out dir> [threads]\n");
        return 2;
    }
    const std::string modelPath = argv[1], inPath = argv[2], outDir = argv[3];
    const int threads = argc > 4 ? std::max(1, atoi(argv[4])) : std::max(1, std::min(8, (int)std::thread::hardware_concurrency() / 2));  // 조각마다 메모리를 꽤 써서 8개까지만
    try {
        auto t0 = std::chrono::steady_clock::now();
        Eigen::MatrixXf audio = loadAudio(inPath);
        printf("loaded %d samples (%.1f s)\n", (int)audio.cols(), audio.cols() / 44100.0);
        fflush(stdout);
        demucscpp::demucs_model model{};
        if (!demucscpp::load_demucs_model(modelPath, &model)) {
            fprintf(stderr, "모델을 읽을 수 없습니다: %s\n", modelPath.c_str());
            return 1;
        }
        Eigen::Tensor3dXf out = separate(model, audio, threads);
        fs::create_directories(fs::u8path(outDir));
        // target 0 drums, 1 bass, 2 other, 3 vocals (6-source 는 4 guitar, 5 piano)
        std::vector<int> inst = {0, 1, 2};
        if (!model.is_4sources) { inst.push_back(4); inst.push_back(5); }
        saveStereo(outDir + "/vocals.wav", out, {3});
        saveStereo(outDir + "/no_vocals.wav", out, inst);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        printf("done %lld ms\n", (long long)ms);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "오류: %s\n", e.what());
        return 1;
    }
}
