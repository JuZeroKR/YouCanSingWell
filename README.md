# YouCanSingWell

노래 연습 프로그램. 마이크 음정을 실시간으로 보여 주고, 립트릴 · 음계 연습을 목표 음 선 위에서 할 수 있다.
유튜브 노래를 가져오면 PC 안에서 보컬을 분리해 멜로디 음정 선을 뽑고, 따라 부르면 내 음정을 겹쳐 보여 준다.

[YouShadow](https://github.com/JuZeroKR/YouShadow) 의 오디오 · 음높이 추출 코드를 바탕으로 만들었다. AI 호출은 없다.

## 기능 (v0.1)

- **실시간 음정**: 마이크 소리의 음높이(F0) 를 10 ms 마다 뽑아 피아노 건반 눈금 위에 그린다. 지금 음(예: `A3 +12`) 과 센트 미터
- **연습 탭**
  - 자유: 그냥 음정만 본다
  - 사이렌: 낮은 음 ↔ 높은 음을 천천히 오르내리는 목표 선을 따라간다 (립트릴에 좋다)
  - 5음 음계: 도레미파솔파미레도를 한 번 부를 때마다 반음씩 올라간다
  - 지속음: 한 음을 길게 붙들고 흔들림을 본다
  - 안내음으로 목표 음을 들려주고, 목표와의 차이(센트) 를 색으로 보여 주며 평균 오차와 맞은 비율을 낸다
- **노래 따라 부르기 탭**
  - 유튜브 주소를 넣으면 yt-dlp 로 소리를 받고, **PC 안에서 보컬을 분리**해 (demucs.cpp, Hybrid Transformer Demucs 4-source, 모델 약 80MB 한 번 다운로드) 반주와 보컬을 나눈다. AI 서비스 호출은 없다
  - 보컬에서 멜로디 음정 선을 뽑아 저장하고, 반주(와 원하는 만큼의 원곡 보컬) 를 틀어 주며 따라 부르면 내 음정이 멜로디 위에 겹쳐 흐른다
  - 이어폰을 쓰면 내 목소리가 함께 들린다 (모니터링, 음량 조절). 옥타브 무시 옵션으로 원곡 가수와 음역이 달라도 맞는 것으로 친다
  - 멜로디와의 차이를 색으로, ±50 센트 안에 든 비율과 평균 오차를 점수로 보여 준다. 그래프를 클릭하면 그 위치로 이동
  - 보컬 분리는 CPU 에서 노래 길이의 1~2배쯤 걸린다 (8 스레드 기준). 한 번 분리한 노래는 바로 열린다

## 빌드 (Windows)

```
powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\youcansingwell.exe
```

`setup-deps.ps1` 은 라이브러리와 함께 demucs.cpp 와 yt-dlp · ffmpeg · deno 도 받아 `build\Release\bin\` 에 둔다 (노래 가져오기에 필요). 보컬 분리 모델(80MB) 은 앱이 처음 노래를 가져올 때 받는다.

## 구조

```
src/
  gui_main.cpp      ImGui 앱: 연습 탭(음정 그래프, 목표 선, 안내음), 노래 탭(가져오기, 멜로디 선, 재생 · 모니터링, 점수)
  pitch.cpp         NCCF 음높이 추출 (실시간 스트리밍 + 파일 전체), 음 이름 · 센트
  audio.cpp         miniaudio: 마이크 입력, 안내음, 노래 재생 + 마이크 모니터링 겸용 장치(SongEngine), wav 읽기 · 쓰기
  song.cpp          노래 파이프라인: yt-dlp → mix.wav → ycs_separate → vocals/no_vocals.wav → melody.json
  separate_main.cpp 보컬 분리 CLI (demucs.cpp 라이브러리 + miniaudio 입출력, 조각 병렬 처리)
  paths.cpp         데이터 경로, 외부 도구 실행
third_party/demucs.cpp  Demucs v4 추론 라이브러리 (MIT, Eigen 헤더 전용) — setup-deps 가 받는다
```

## 다음 단계

- 연습 기록 저장 (SQLite) 과 노래별 점수 추이
- 가사 표시, 구간 반복, 키(조) 바꾸기
- 설치 패키지 · GitHub 릴리스 자동 빌드

## AI 사용 표기

설계와 코드는 Anthropic 의 **Claude (Claude Code)** 와 대화하며 작성했다.

## 라이선스

MIT
