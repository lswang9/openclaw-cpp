cmake_minimum_required(VERSION 3.20)
project(openclaw-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)

add_executable(openclaw-cpp src/main.cpp)
target_include_directories(openclaw-cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openclaw-cpp PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)

install(TARGETS openclaw-cpp RUNTIME DESTINATION bin)
