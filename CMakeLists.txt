
cmake_minimum_required(VERSION 3.16)
project(roi_ocr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── M1 Mac: point CMake to Homebrew (arm64) ───
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/opencv")

# ── OpenCV (has proper CMake config via Homebrew) ─────────────────
find_package(OpenCV REQUIRED)

# ── Tesseract — no .pc file shipped, link manually ───────────────
find_path(TESSERACT_INCLUDE_DIR
    NAMES tesseract/baseapi.h
    PATHS /opt/homebrew/include
          /opt/homebrew/opt/tesseract/include
    REQUIRED
)
find_library(TESSERACT_LIB
    NAMES tesseract
    PATHS /opt/homebrew/lib
          /opt/homebrew/opt/tesseract/lib
    REQUIRED
)
find_library(ARCHIVE_LIB NAMES archive PATHS /opt/homebrew/opt/libarchive/lib REQUIRED)
find_library(CURL_LIB    NAMES curl    PATHS /opt/homebrew/lib REQUIRED)

# ── Leptonica — link by actual library name, not pkg-config name ──
find_path(LEPT_INCLUDE_DIR
    NAMES leptonica/allheaders.h
    PATHS /opt/homebrew/include
          /opt/homebrew/opt/leptonica/include
    REQUIRED
)
find_library(LEPT_LIB
    NAMES leptonica
    PATHS /opt/homebrew/lib
          /opt/homebrew/opt/leptonica/lib
    REQUIRED
)

message(STATUS "Tesseract include: ${TESSERACT_INCLUDE_DIR}")
message(STATUS "Tesseract lib:     ${TESSERACT_LIB}")
message(STATUS "Leptonica include: ${LEPT_INCLUDE_DIR}")
message(STATUS "Leptonica lib:     ${LEPT_LIB}")

# ── nlohmann/json — header-only, fetched automatically ───────────
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
  DOWNLOAD_NO_EXTRACT TRUE
  DOWNLOAD_DIR ${CMAKE_BINARY_DIR}/include/nlohmann
)
FetchContent_MakeAvailable(nlohmann_json)

# ── Target ────────────────────────────────────────────────────────
add_executable(roi_ocr roi_ocr.cpp)

target_include_directories(roi_ocr PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${TESSERACT_INCLUDE_DIR}
    ${LEPT_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/include
)

target_link_libraries(roi_ocr PRIVATE
    ${OpenCV_LIBS}
    ${TESSERACT_LIB}
    ${ARCHIVE_LIB}
    ${CURL_LIB}
    ${LEPT_LIB}
)

