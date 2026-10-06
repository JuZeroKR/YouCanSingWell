# YouCanSingWell

노래 연습 프로그램. 마이크 음정을 실시간으로 보여 주고, 립트릴 · 음계 연습을 목표 음 선 위에서 할 수 있다.
다음 단계로 유튜브 노래를 가져와 보컬 음정 선을 뽑고(PC 안에서 보컬 분리), 따라 부르면 내 음정을 겹쳐 보여 준다.

[YouShadow](https://github.com/JuZeroKR/YouShadow) 의 오디오 · 음높이 추출 코드를 바탕으로 만들었다. AI 호출은 없다.

## 지금 되는 것 (v0.1)

- **실시간 음정**: 마이크 소리의 음높이(F0) 를 10 ms 마다 뽑아 피아노 건반 눈금 위에 그린다. 지금 음(예: `A3 +12`) 과 센트 미터
- **연습**
  - 자유: 그냥 음정만 본다
  - 사이렌: 낮은 음 ↔ 높은 음을 천천히 오르내리는 목표 선을 따라간다 (립트릴에 좋다)
  - 5음 음계: 도레미파솔파미레도를 한 번 부를 때마다 반음씩 올라간다
  - 지속음: 한 음을 길게 붙들고 흔들림을 본다
- **안내음**: 목표 음을 부드러운 톤으로 들려준다 (켜고 끄기)
- 연습 중에는 목표와 내 음의 차이(센트) 를 색으로 보여 주고, 평균 오차와 맞은 비율을 낸다

## 빌드 (Windows)

```
powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\youcansingwell.exe
```

## 구조

```
src/
  gui_main.cpp   ImGui 앱: 음정 그래프, 연습 모드, 안내음
  pitch.cpp      NCCF 음높이 추출 (실시간 스트리밍 + 파일 전체), 음 이름 · 센트
  audio.cpp      miniaudio: 마이크 입력, 안내음 생성, wav 읽기 · 쓰기
  paths.cpp      데이터 경로, 외부 도구 실행
```

## 다음 단계

- 유튜브 노래 가져오기 (yt-dlp) → 보컬 분리 (demucs.cpp, 오프라인) → 보컬 음정 선
- 따라 부르기: 노래 재생 + 내 음정 겹쳐 보기 + 이어폰으로 내 목소리 모니터링
- 연습 기록 저장 (SQLite)

## AI 사용 표기

설계와 코드는 Anthropic 의 **Claude (Claude Code)** 와 대화하며 작성했다.

## 라이선스

MIT
