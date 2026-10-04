cmake_minimum_required(VERSION 3.16)
project(frameeyeosc-panel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The panel ships in the same tarball as frameeyeosc, so it takes its version from Cargo.toml
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../Cargo.toml" CARGO_VERSION_LINE REGEX "^version = \"[^\"]+\"" LIMIT_COUNT 1)
string(REGEX REPLACE "^version = \"([^\"]+)\".*" "\\1" FRAMEEYEOSC_VERSION "${CARGO_VERSION_LINE}")
if(NOT FRAMEEYEOSC_VERSION)
  message(FATAL_ERROR "Could not read the version from ../Cargo.toml")
endif()
# The update checker shared with the other Frame apps (copied by frame-updater's sync.sh)
set(FRAME_UPDATER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/frame-updater")

# The OpenVR library that ships with SteamVR (its path on the Frame's SteamOS)
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory containing libopenvr_api.so")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)

add_executable(frameeyeosc-panel
  src/main.cpp
  src/auto_recenter.cpp
  src/autostart.cpp
  src/changelog.cpp
  src/command.cpp
  src/config.cpp
  src/draw.cpp
  src/eyecam.cpp
  src/gaze_dots.cpp
  src/gaze_fit.cpp
  src/fit_text.cpp
  src/host_entry.cpp
  src/i18n.cpp
  src/json.cpp
  src/model.cpp
  src/panel.cpp
  src/recorder.cpp
  src/setup_tools.cpp
  src/sounds.cpp
  src/status.cpp
  src/target.cpp
  src/theme.cpp
  src/vk_texture.cpp
  src/vr_overlay.cpp
  ${FRAME_UPDATER_DIR}/cpp/update_check.cpp
)
target_include_directories(frameeyeosc-panel PRIVATE third_party/openvr ${FRAME_UPDATER_DIR}/cpp)
target_compile_definitions(frameeyeosc-panel PRIVATE FRAMEEYEOSC_VERSION="${FRAMEEYEOSC_VERSION}")
target_compile_options(frameeyeosc-panel PRIVATE -Wall -Wextra)
target_link_libraries(frameeyeosc-panel PRIVATE
  PkgConfig::CAIRO
  PkgConfig::FREETYPE
  PkgConfig::VULKAN
  Threads::Threads
  "${OPENVR_LIB_DIR}/libopenvr_api.so"
)
# Use SteamVR's own library at run time too
set_target_properties(frameeyeosc-panel PROPERTIES
  BUILD_RPATH "${OPENVR_LIB_DIR}"
  INSTALL_RPATH "${OPENVR_LIB_DIR}"
)

install(TARGETS frameeyeosc-panel RUNTIME DESTINATION bin)

# The gaze fit's logic, tested without OpenVR: panel/build/gaze-fit-test (or ctest)
add_executable(gaze-fit-test tests/gaze_fit_test.cpp src/gaze_fit.cpp src/status.cpp src/json.cpp)
target_include_directories(gaze-fit-test PRIVATE src)
target_compile_options(gaze-fit-test PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME gaze-fit COMMAND gaze-fit-test)

# The debug gaze dots' packet, geometry and socket, tested without OpenVR: panel/build/gaze-dots-test
add_executable(gaze-dots-test tests/gaze_dots_test.cpp src/gaze_dots.cpp src/gaze_fit.cpp src/status.cpp src/json.cpp)
target_include_directories(gaze-dots-test PRIVATE src)
target_compile_options(gaze-dots-test PRIVATE -Wall -Wextra)
add_test(NAME gaze-dots COMMAND gaze-dots-test)

# The eye fit's sound cues (the WAV files and which cue goes with which moment), without playing: panel/build/sounds-test
add_executable(sounds-test tests/sounds_test.cpp src/sounds.cpp src/gaze_fit.cpp src/status.cpp src/json.cpp)
target_include_directories(sounds-test PRIVATE src)
target_compile_options(sounds-test PRIVATE -Wall -Wextra)
add_test(NAME sounds COMMAND sounds-test)

# Re-centering by itself when the headset is put on (when it arms and fires), without OpenVR:
# panel/build/auto-recenter-test
add_executable(auto-recenter-test tests/auto_recenter_test.cpp src/auto_recenter.cpp)
target_include_directories(auto-recenter-test PRIVATE src)
target_compile_options(auto-recenter-test PRIVATE -Wall -Wextra)
add_test(NAME auto-recenter COMMAND auto-recenter-test)

# The eye log: starting, stopping and reaping the recording child (a stand-in, or a real frameeyeosc given as the
# argument): panel/build/recorder-test [target/release/frameeyeosc]
add_executable(recorder-test tests/recorder_test.cpp src/recorder.cpp)
target_include_directories(recorder-test PRIVATE src)
target_compile_options(recorder-test PRIVATE -Wall -Wextra)
add_test(NAME recorder COMMAND recorder-test)

# The shared rules: what an eye fit writes and "Reset" clears, the re-wear fit auto_recenter asks for, the output
# cards' arguments: panel/build/model-test
add_executable(model-test tests/model_test.cpp src/model.cpp src/config.cpp src/json.cpp src/gaze_fit.cpp
  src/status.cpp)
target_include_directories(model-test PRIVATE src ${FRAME_UPDATER_DIR}/cpp)
target_compile_options(model-test PRIVATE -Wall -Wextra)
add_test(NAME model COMMAND model-test)

# The Eyelids and eye cameras tabs as drawn (案E: the widening slider's routing, "Fine-tune", the moved sensitivity
# slider, a setting's slider): panel/build/panel-test, run right after it is built (it renders with the panel's fonts)
add_executable(panel-test tests/panel_test.cpp
  src/autostart.cpp
  src/changelog.cpp
  src/command.cpp
  src/config.cpp
  src/draw.cpp
  src/eyecam.cpp
  src/fit_text.cpp
  src/gaze_fit.cpp
  src/host_entry.cpp
  src/i18n.cpp
  src/json.cpp
  src/model.cpp
  src/panel.cpp
  src/recorder.cpp
  src/setup_tools.cpp
  src/status.cpp
  src/theme.cpp
  ${FRAME_UPDATER_DIR}/cpp/update_check.cpp
)
target_include_directories(panel-test PRIVATE src ${FRAME_UPDATER_DIR}/cpp)
target_compile_definitions(panel-test PRIVATE FRAMEEYEOSC_VERSION="${FRAMEEYEOSC_VERSION}")
target_compile_options(panel-test PRIVATE -Wall -Wextra)
target_link_libraries(panel-test PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE Threads::Threads)
add_custom_command(TARGET panel-test POST_BUILD COMMAND panel-test VERBATIM)
add_test(NAME panel COMMAND panel-test)

# Checking a typed target PC and the eye fit's failure texts (both languages): panel/build/text-test
add_executable(text-test tests/text_test.cpp src/host_entry.cpp src/fit_text.cpp src/i18n.cpp src/gaze_fit.cpp
  src/status.cpp src/json.cpp)
target_include_directories(text-test PRIVATE src)
target_compile_options(text-test PRIVATE -Wall -Wextra)
add_test(NAME text COMMAND text-test)

# Reading the version history from CHANGELOG.md / CHANGELOG.ja.md (both formats, first sentences, markdown, the
# English fallback): panel/build/changelog-test
add_executable(changelog-test tests/changelog_test.cpp src/changelog.cpp)
target_include_directories(changelog-test PRIVATE src)
target_compile_options(changelog-test PRIVATE -Wall -Wextra)
add_test(NAME changelog COMMAND changelog-test)

# The eye cameras tab (eyecam-rec): its status file, the setup checklist, when the tab and the full-view light show,
# the step texts, the setup's Konsole command lines, and the control socket against a stand-in recorder in /tmp:
# panel/build/eyecam-test
add_executable(eyecam-test tests/eyecam_test.cpp src/eyecam.cpp src/i18n.cpp src/json.cpp src/setup_tools.cpp)
target_include_directories(eyecam-test PRIVATE src)
target_compile_options(eyecam-test PRIVATE -Wall -Wextra)
target_link_libraries(eyecam-test PRIVATE Threads::Threads)
add_test(NAME eyecam COMMAND eyecam-test)
