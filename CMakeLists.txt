cmake_minimum_required(VERSION 3.20)
project(openclaw-cpp VERSION 1.52.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(OPENCLAW_SOURCES
  src/main.cpp
  src/agent/loop.cpp
  src/agent/files.cpp
  src/agent/routing.cpp
  src/agent/pool.cpp
  src/acp/session.cpp
  src/acp/bindings.cpp
  src/board/store.cpp
  src/canvas/store.cpp
  src/acp/wire.cpp
  src/sim/backend.cpp
  src/sim/capture.cpp
  src/sim/input.cpp
  src/browser/cdp_client.cpp
  src/browser/engine.cpp
  src/voice/tts.cpp
  src/channels/feishu.cpp
  src/channels/feishu_ws.cpp
  src/channels/slack.cpp
  src/channels/slack_interactive.cpp
  src/channels/signing.cpp
  src/channels/slack_socket.cpp
  src/channels/discord.cpp
  src/channels/discord_commands.cpp
  src/channels/discord_gateway.cpp
  src/channels/simple_inbound.cpp
  src/channels/whatsapp.cpp
  src/daemon/lifecycle.cpp
  src/provider/deepseek.cpp
  src/provider/mock.cpp
  src/provider/openai_compat.cpp
  src/provider/router.cpp
  src/tools/registry.cpp
  src/tools/runenv.cpp
  src/tools/exec.cpp
  src/workspace/workspace.cpp
  src/workspace/embeddings.cpp
  src/context/system_prompt.cpp
  src/context/compaction.cpp
  src/tokenizer/bpe.cpp
  src/session/store.cpp
  src/gateway/server.cpp
  src/gateway/methods.cpp
  src/gateway/catalog_generated.cpp
  src/gateway/extra.cpp
  src/config/config.cpp
  src/config/validate.cpp
  src/channels/registry.cpp
  src/channels/adapter.cpp
  src/channels/outbound.cpp
  src/memory/store.cpp
  src/cron/scheduler.cpp
  src/auth/gateway_auth.cpp
  src/auth/device_link.cpp
  src/skills/loader.cpp
  src/plugins/registry.cpp
  src/plugins/js_host.cpp
  src/plugins/js_extensions.cpp
  src/mcp/client.cpp
  src/mcp/bridge.cpp
  src/mcp/stdio.cpp
  src/approvals/store.cpp
  src/secrets/store.cpp
  src/events/bus.cpp
  src/nodes/registry.cpp
  src/media/store.cpp
  src/context/assemble.cpp
  src/logging/tail.cpp
  src/persist/store.cpp
  src/diagnostics/doctor.cpp
)

add_executable(openclaw-cpp ${OPENCLAW_SOURCES})
target_include_directories(openclaw-cpp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway)
target_link_libraries(openclaw-cpp PRIVATE
  nlohmann_json::nlohmann_json
  CURL::libcurl
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  ${CMAKE_DL_LIBS})

# Sample native plugin (.so) — Linux
add_library(openclaw_sample_echo SHARED plugins/sample_echo/plugin.c)
target_include_directories(openclaw_sample_echo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(openclaw_sample_echo PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/plugins/sample_echo
  OUTPUT_NAME openclaw_sample_echo
  PREFIX "lib")

install(TARGETS openclaw-cpp RUNTIME DESTINATION bin)
install(FILES web/control.html DESTINATION share/openclaw-cpp OPTIONAL)
install(FILES include/openclaw/plugins/sdk.h DESTINATION include/openclaw/plugins)
enable_testing()
add_test(NAME self_test COMMAND openclaw-cpp self-test)
