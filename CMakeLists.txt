cmake_minimum_required(VERSION 3.15)
project(AudioWarden VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages provided by vcpkg
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(taglib CONFIG REQUIRED)

add_executable(audio-warden
    main.cpp
    audio_warden.cpp
    io_utils.cpp
    prompter.cpp
    scanner.cpp
    tag_debug.cpp
    validator.cpp
)
target_link_libraries(audio-warden PRIVATE CLI11::CLI11 nlohmann_json::nlohmann_json TagLib::tag)
