cmake_minimum_required(VERSION 3.20)
project(SecVMS VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- Qt6 ---
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)

# --- FFmpeg (из MSYS2 ucrt64) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)

add_executable(SecVMS WIN32
    main.cpp
    MainWindow.cpp MainWindow.h
    LiveView.cpp LiveView.h
    VideoCell.cpp VideoCell.h
    VideoWall.cpp VideoWall.h
    XmClient.cpp XmClient.h
    Discovery.cpp Discovery.h
    Journal.cpp Journal.h
    Errors.cpp Errors.h
    Types.h
    app.rc
)

target_link_libraries(SecVMS PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent
    PkgConfig::FFMPEG crypt32 winmm iphlpapi ws2_32)

target_compile_definitions(SecVMS PRIVATE
    SECVMS_VERSION="${PROJECT_VERSION}")
