cmake_minimum_required(VERSION 3.20)
project(openclaw-cpp VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(OPENCLAW_SOURCES
  src/main.cpp
  src/agent/loop.cpp
  src/provider/deepseek.cpp
  src/provider/mock.cpp
  src/tools/registry.cpp
  src/session/store.cpp
  src/gateway/server.cpp
  src/gateway/methods.cpp
  src/gateway/catalog_generated.cpp
  src/config/config.cpp
  src/channels/registry.cpp
  src/channels/adapter.cpp
  src/memory/store.cpp
  src/cron/scheduler.cpp
  src/auth/gateway_auth.cpp
  src/skills/loader.cpp
  src/plugins/registry.cpp
  src/mcp/client.cpp
  src/approvals/store.cpp
  src/secrets/store.cpp
)

add_executable(openclaw-cpp ${OPENCLAW_SOURCES})
target_include_directories(openclaw-cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openclaw-cpp PRIVATE nlohmann_json::nlohmann_json CURL::libcurl Threads::Threads)

install(TARGETS openclaw-cpp RUNTIME DESTINATION bin)
enable_testing()
add_test(NAME self_test COMMAND openclaw-cpp self-test)
