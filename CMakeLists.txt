cmake_minimum_required(VERSION 3.20)
project(YouCanSingWell VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 배포판이 VC 런타임 없이 동작하도록 CRT 를 정적으로 링크한다
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# ---- 공통 옵션 ----
add_library(ycs_options INTERFACE)
target_compile_definitions(ycs_options INTERFACE YCS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(ycs_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(APPLE)
    target_compile_definitions(ycs_options INTERFACE GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    target_compile_options(ycs_options INTERFACE /utf-8 /W3)
endif()

# ---- SQLite ----
add_library(sqlite3 STATIC ${TP}/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${TP}/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
endif()

# ---- 핵심 라이브러리 (오디오, 음높이, 경로) ----
add_library(ycs_core STATIC
    src/paths.cpp
    src/audio.cpp
    src/pitch.cpp
    src/song.cpp
)
target_include_directories(ycs_core PUBLIC ${TP} src)
target_link_libraries(ycs_core PUBLIC ycs_options sqlite3)

# ---- demucs.cpp (보컬 분리, Eigen 헤더 전용). 자체 CMake 는 gcc 전용 플래그라 여기서 직접 빌드한다 ----
set(DEMUCS ${TP}/demucs.cpp)
file(GLOB DEMUCS_SOURCES ${DEMUCS}/src/*.cpp)
add_library(demucs STATIC ${DEMUCS_SOURCES})
target_include_directories(demucs PUBLIC ${DEMUCS}/src ${DEMUCS}/vendor/eigen)
target_compile_definitions(demucs PUBLIC NDEBUG EIGEN_MPL2_ONLY)
target_link_libraries(demucs PUBLIC ycs_options)
if(MSVC)
    target_compile_options(demucs PRIVATE /O2 /fp:fast /arch:AVX2 /bigobj /w)  # AVX2: Eigen 행렬 연산이 2~3배 빨라진다 (2013년 이후 CPU)
else()
    target_compile_options(demucs PRIVATE -O3 -ffast-math -w)
endif()

# 보컬 분리 CLI: 앱이 백그라운드로 실행한다 (ycs_separate <model.bin> <input.wav> <out dir> [threads])
add_executable(ycs_separate src/separate_main.cpp)
target_link_libraries(ycs_separate PRIVATE ycs_core demucs)
if(MSVC)
    target_compile_options(ycs_separate PRIVATE /bigobj /arch:AVX2)
endif()

# ---- Dear ImGui + GLFW 백엔드 ----
set(IMGUI ${TP}/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_glfw.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(imgui PUBLIC ycs_options)
if(WIN32)
    target_include_directories(imgui PUBLIC ${TP}/glfw/include)
else()
    find_package(glfw3 3.3 REQUIRED)   # brew install glfw
    target_link_libraries(imgui PUBLIC glfw)
endif()

# ---- GUI ----
if(WIN32)
    add_executable(youcansingwell WIN32 src/gui_main.cpp assets/youcansingwell.rc)
    target_link_libraries(youcansingwell PRIVATE
        ycs_core imgui
        ${TP}/glfw/lib-vc2022/glfw3_mt.lib
        opengl32
    )
    target_link_options(youcansingwell PRIVATE /ENTRY:mainCRTStartup)
else()
    add_executable(youcansingwell src/gui_main.cpp)
    target_link_libraries(youcansingwell PRIVATE ycs_core imgui "-framework OpenGL")
endif()
