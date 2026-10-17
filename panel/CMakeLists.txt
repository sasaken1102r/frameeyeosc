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
# The other Frame apps' list and their installer in Konsole, shared with the other Frame apps (copied by frame-apps'
# sync.sh)
set(FRAME_APPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/frame-apps")
set(FRAME_APPS_SOURCES
  ${FRAME_APPS_DIR}/cpp/frame_apps.cpp
  ${FRAME_APPS_DIR}/cpp/frame_apps_data.cpp
  ${FRAME_APPS_DIR}/cpp/frame_apps_catalog.cpp
  ${FRAME_APPS_DIR}/cpp/frame_apps_icons.cpp
  ${FRAME_APPS_DIR}/cpp/frame_apps_cairo.cpp
)

# The OpenVR library that ships with SteamVR (its path on the Frame's SteamOS)
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory containing libopenvr_api.so")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)

set(PANEL_UI_SOURCES
  src/autostart.cpp
  src/changelog.cpp
  src/command.cpp
  src/config.cpp
  src/diag.cpp
  src/draw.cpp
  src/eyecam.cpp
  src/eye_preview.cpp
  src/fit_text.cpp
  src/gaze_fit.cpp
  src/host_entry.cpp
  src/i18n.cpp
  src/icons.cpp
  src/json.cpp
  src/model.cpp
  src/panel.cpp
  src/recorder.cpp
  src/report.cpp
  src/setup_tools.cpp
  src/status.cpp
  src/text_layout.cpp
  src/theme.cpp
  src/ui_state.cpp
  ${FRAME_UPDATER_DIR}/cpp/update_check.cpp
  ${FRAME_APPS_SOURCES}
)

add_executable(frameeyeosc-panel
  src/main.cpp
  src/auto_recenter.cpp
  src/gaze_dots.cpp
  src/sounds.cpp
  src/target.cpp
  src/vk_texture.cpp
  src/vr_overlay.cpp
  ${PANEL_UI_SOURCES}
)
target_include_directories(frameeyeosc-panel PRIVATE third_party/openvr ${FRAME_UPDATER_DIR}/cpp ${FRAME_APPS_DIR}/cpp)
target_compile_definitions(frameeyeosc-panel PRIVATE FRAMEEYEOSC_VERSION="${FRAMEEYEOSC_VERSION}")
target_compile_options(frameeyeosc-panel PRIVATE -Wall -Wextra)
# Keep release builds compact without removing features or changing debug builds.
option(FRAMEEYEOSC_SIZE_OPTIMIZED "Optimize the release panel for installed size" ON)
if(FRAMEEYEOSC_SIZE_OPTIMIZED)
  target_compile_options(frameeyeosc-panel PRIVATE
    $<$<CONFIG:Release>:-Os;-ffunction-sections;-fdata-sections>)
  target_link_options(frameeyeosc-panel PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT PANEL_IPO_SUPPORTED OUTPUT PANEL_IPO_ERROR)
  if(PANEL_IPO_SUPPORTED)
    set_property(TARGET frameeyeosc-panel PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endif()
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

include(CTest)
if(BUILD_TESTING)
function(add_panel_test name)
  string(REPLACE "-" "_" stem "${name}")
  add_executable(${name}-test tests/${stem}_test.cpp ${ARGN})
  target_include_directories(${name}-test PRIVATE src ${FRAME_UPDATER_DIR}/cpp ${FRAME_APPS_DIR}/cpp)
  target_compile_options(${name}-test PRIVATE -Wall -Wextra)
  target_link_libraries(${name}-test PRIVATE Threads::Threads)
  add_test(NAME ${name} COMMAND ${name}-test)
endfunction()

# The gaze fit's logic, tested without OpenVR: panel/build/gaze-fit-test (or ctest)
add_panel_test(gaze-fit src/gaze_fit.cpp src/status.cpp src/json.cpp)

# The debug gaze dots' packet, geometry and socket, tested without OpenVR: panel/build/gaze-dots-test
add_panel_test(gaze-dots src/gaze_dots.cpp src/gaze_fit.cpp src/status.cpp src/json.cpp)

# The eye preview without cairo or OpenVR: which values drive the drawn eyes, the lids' and pupil's geometry, eyecam-rec's
# preview file (made-up pictures in a folder under /tmp: its checks, the seqlock, a replaced or removed file), when the
# pictures can show and "preview on" / "off": panel/build/eye-preview-test
add_panel_test(eye-preview src/eye_preview.cpp src/gaze_dots.cpp src/eyecam.cpp
  src/i18n.cpp src/json.cpp src/setup_tools.cpp src/recorder.cpp)

# The eye fit's sound cues (the WAV files and which cue goes with which moment), without playing: panel/build/sounds-test
add_panel_test(sounds src/sounds.cpp src/gaze_fit.cpp src/status.cpp src/json.cpp)

# Re-centering by itself when the headset is put on (when it arms and fires), without OpenVR:
# panel/build/auto-recenter-test
add_panel_test(auto-recenter src/auto_recenter.cpp)

# The eye log: starting, stopping and reaping the recording child (a stand-in, or a real frameeyeosc given as the
# argument): panel/build/recorder-test [target/release/frameeyeosc]
add_panel_test(recorder src/recorder.cpp)

# The shared rules: what an eye fit writes and "Reset" clears, the re-wear fit auto_recenter asks for, the output
# cards' arguments: panel/build/model-test
add_panel_test(model src/model.cpp src/config.cpp src/json.cpp src/gaze_fit.cpp
  src/status.cpp)

# The Eyelids and eye cameras tabs as drawn (案E: the widening slider's routing, "Fine-tune", the moved sensitivity
# slider, a setting's slider), the other Frame apps' row, list and confirmation, and the eye preview's sheet: panel/build/panel-test, run right after it is built (it renders with the panel's fonts)
add_panel_test(panel ${PANEL_UI_SOURCES})
target_compile_definitions(panel-test PRIVATE FRAMEEYEOSC_VERSION="${FRAMEEYEOSC_VERSION}")
target_link_libraries(panel-test PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE Threads::Threads)
add_custom_command(TARGET panel-test POST_BUILD COMMAND panel-test VERBATIM)

# Checking a typed target PC, the eye fit's failure texts (both languages) and that no text has an arrow or symbol as a
# character (icon markers instead): panel/build/text-test
add_panel_test(text src/host_entry.cpp src/fit_text.cpp src/i18n.cpp src/icons.cpp
  src/gaze_fit.cpp src/status.cpp src/json.cpp)

# Reading the version history from CHANGELOG.md / CHANGELOG.ja.md (both formats, first sentences, markdown, the
# English fallback): panel/build/changelog-test
add_panel_test(changelog src/changelog.cpp)

# The eye cameras tab (eyecam-rec): its status file, the setup checklist, when the tab and the full-view light show,
# the step texts, the setup's Konsole command lines, and the control socket against a stand-in recorder in /tmp:
# panel/build/eyecam-test
add_panel_test(eyecam src/eyecam.cpp src/i18n.cpp src/json.cpp src/setup_tools.cpp
  src/recorder.cpp)
# (eyecam's own protocol files, read as the panel reads the installed copies)
target_compile_definitions(eyecam-test PRIVATE EYECAM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/eyecam")

# The diagnostics page's texts and code (diag.{h,cpp}: the cards, the diagnostic code, SteamOS's version, SHA-256),
# without cairo: panel/build/diag-test
add_panel_test(diag src/diag.cpp src/eyecam.cpp src/i18n.cpp src/json.cpp src/status.cpp
  src/model.cpp src/config.cpp src/gaze_fit.cpp src/setup_tools.cpp src/recorder.cpp)

# The records of calibrations and eye fits (report.{h,cpp}: parsing the journal and Valve's log, the key lines, the
# files, keeping 10, report.txt, --report) and the remembered sub-tab (ui_state), with made-up logs in a folder under
# the current one: panel/build/report-test
add_panel_test(report src/report.cpp src/ui_state.cpp src/i18n.cpp src/icons.cpp
  src/json.cpp src/command.cpp)

add_panel_test(io-json src/json.cpp)
endif()
