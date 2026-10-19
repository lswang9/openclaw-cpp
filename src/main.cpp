// openclaw-cpp — C++ rewrite toward OpenClaw 2026.10.5 full parity.
#include "openclaw/agent/loop.hpp"
#include "openclaw/gateway/server.hpp"
#include "openclaw/provider/deepseek.hpp"
#include "openclaw/provider/mock.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/tools/registry.hpp"
#include "openclaw/util.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/memory/store.hpp"
#include "openclaw/cron/scheduler.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string JoinArgs(int argc, char** argv, int start) {
    std::ostringstream oss;
    for (int i = start; i < argc; ++i) {
        if (i > start) {
            oss << ' ';
        }
        oss << argv[i];
    }
    return oss.str();
}

std::string ReadStdin() {
    std::ostringstream oss;
    oss << std::cin.rdbuf();
    std::string s = oss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void PrintUsage() {
    std::cerr
        << "openclaw-cpp — C++ rewrite (align: OpenClaw 2026.10.5)\n"
        << "usage:\n"
        << "  openclaw-cpp chat [-s SESSION] <message...>\n"
        << "  openclaw-cpp gateway [--port N] [--host ADDR]\n"
        << "  openclaw-cpp tools\n"
        << "  openclaw-cpp channels\n"
        << "  openclaw-cpp config\n"
        << "  openclaw-cpp methods\n"
        << "  openclaw-cpp memory [set KEY VAL | get KEY]\n"
        << "  openclaw-cpp cron\n"
        << "  openclaw-cpp status\n"
        << "  openclaw-cpp self-test\n"
        << "  openclaw-cpp <message...>          # shorthand for chat\n";
}

int CmdSelfTest() {
    using namespace openclaw;
    int failures = 0;

    // 1) echo tool
    auto registry = tools::MakeDefaultRegistry();
    const auto echo_out = registry.Invoke("echo", {{"text", "ping"}});
    if (echo_out.find("ping") == std::string::npos) {
        std::cerr << "FAIL echo tool\n";
        ++failures;
    } else {
        std::cerr << "PASS echo tool\n";
    }

    // 2) agent loop with mock: tool_calls then final stop
    provider::MockProvider mock;
    {
        agent::CompletionResult first;
        first.ok = true;
        first.finish_reason = "tool_calls";
        first.message.role = "assistant";
        first.message.content = "";
        first.message.tool_calls = nlohmann::json::array({
            {{"id", "call_1"},
             {"type", "function"},
             {"function", {{"name", "echo"}, {"arguments", "{\"text\":\"hello-loop\"}"}}}},
        });
        agent::ToolCall tc;
        tc.id = "call_1";
        tc.name = "echo";
        tc.arguments_json = "{\"text\":\"hello-loop\"}";
        first.tool_calls.push_back(tc);
        mock.Enqueue(first);

        agent::CompletionResult second;
        second.ok = true;
        second.finish_reason = "stop";
        second.message.role = "assistant";
        second.message.content = "loop-ok:hello-loop";
        mock.Enqueue(second);
    }

    std::vector<agent::Message> msgs;
    msgs.push_back(agent::Message{.role = "user", .content = "please echo hello-loop"});
    auto result = agent::RunAgentLoop(mock, registry, msgs, {.max_turns = 4, .verbose = false});
    if (!result.ok || result.final_text.find("loop-ok") == std::string::npos ||
        result.turns != 2) {
        std::cerr << "FAIL agent loop mock (ok=" << result.ok << " turns=" << result.turns
                  << " text=" << result.final_text << " err=" << result.error << ")\n";
        ++failures;
    } else {
        std::cerr << "PASS agent loop mock (turns=" << result.turns << ")\n";
    }

    // 3) session store round-trip
    const auto tmp = std::filesystem::temp_directory_path() / "openclaw-cpp-selftest";
    session::Store store(tmp);
    store.Save("t1", {agent::Message{.role = "user", .content = "hi"}});
    auto loaded = store.Load("t1");
    if (loaded.size() != 1 || loaded[0].content != "hi") {
        std::cerr << "FAIL session store\n";
        ++failures;
    } else {
        std::cerr << "PASS session store\n";
    }
    store.Remove("t1");

    // 4) tools list includes time_now
    bool has_time = false;
    for (const auto& n : registry.Names()) {
        if (n == "time_now") {
            has_time = true;
        }
    }
    if (!has_time) {
        std::cerr << "FAIL tools registry time_now\n";
        ++failures;
    } else {
        std::cerr << "PASS tools registry\n";
    }

    // 5) channels + methods catalog
    {
        auto ch = channels::MakeDefaultChannels();
        if (ch.List().size() < 3) {
            std::cerr << "FAIL channels registry\n";
            ++failures;
        } else {
            std::cerr << "PASS channels registry\n";
        }
        if (gateway::AdvertisedMethods().empty()) {
            std::cerr << "FAIL methods catalog\n";
            ++failures;
        } else {
            std::cerr << "PASS methods catalog (" << gateway::AdvertisedMethods().size() << ")\n";
        }
    }

    if (failures == 0) {
        std::cerr << "self-test: ALL PASS\n";
        return 0;
    }
    std::cerr << "self-test: " << failures << " failure(s)\n";
    return 1;
}

int CmdChat(int argc, char** argv, int argi) {
    using namespace openclaw;
    std::string session_id;
    while (argi < argc) {
        const std::string a = argv[argi];
        if (a == "-s" || a == "--session") {
            if (argi + 1 >= argc) {
                std::cerr << "error: -s requires id\n";
                return 1;
            }
            session_id = argv[++argi];
            ++argi;
            continue;
        }
        break;
    }

    std::string user_message = JoinArgs(argc, argv, argi);
    if (user_message.empty()) {
        user_message = ReadStdin();
    }
    if (user_message.empty()) {
        std::cerr << "error: empty message\n";
        return 1;
    }

    const std::string api_key = EnvOr("DEEPSEEK_API_KEY", "");
    if (api_key.empty()) {
        std::cerr << "error: set DEEPSEEK_API_KEY (see .env.example)\n";
        return 1;
    }

    const std::string base = EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    const std::string model = EnvOr("DEEPSEEK_MODEL", "deepseek-chat");
    const auto sess_root =
        std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));

    auto registry = tools::MakeDefaultRegistry();
    session::Store store(sess_root);
    provider::DeepSeekProvider provider(api_key, base, model);

    std::vector<agent::Message> messages = store.Load(session_id.empty() ? "_" : session_id);
    messages.push_back(agent::Message{.role = "user", .content = user_message});

    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto result = agent::RunAgentLoop(provider, registry, messages, {.max_turns = 8, .verbose = false});
    curl_global_cleanup();

    if (!session_id.empty() && result.ok) {
        store.Save(session_id, result.messages);
    }

    if (!result.ok) {
        std::cerr << "error: " << result.error << '\n';
        // 402 and friends exit 2 for scripting
        if (result.error.find("402") != std::string::npos) {
            return 2;
        }
        return 1;
    }
    std::cout << result.final_text << '\n';
    return 0;
}

int CmdGateway(int argc, char** argv, int argi) {
    using namespace openclaw;
    gateway::ServerOptions opts;
    opts.bind_host = EnvOr("OPENCLAW_GATEWAY_HOST", "127.0.0.1");
    opts.port = static_cast<std::uint16_t>(std::stoi(EnvOr("OPENCLAW_GATEWAY_PORT", "18789")));
    while (argi < argc) {
        const std::string a = argv[argi++];
        if ((a == "--port" || a == "-p") && argi < argc) {
            opts.port = static_cast<std::uint16_t>(std::stoi(argv[argi++]));
        } else if ((a == "--host") && argi < argc) {
            opts.bind_host = argv[argi++];
        } else if (a == "--verbose" || a == "-v") {
            opts.verbose = true;
        }
    }

    const std::string api_key = EnvOr("DEEPSEEK_API_KEY", "");
    // Allow gateway to start without key (health/status/tools still work; chat returns error)
    const std::string base = EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    const std::string model = EnvOr("DEEPSEEK_MODEL", "deepseek-chat");
    const auto sess_root =
        std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));

    auto registry = tools::MakeDefaultRegistry();
    auto channels = channels::MakeDefaultChannels();
    session::Store store(sess_root);
    auto cfg = config::LoadOrDefault(config::DefaultPath());
    cfg.gateway_host = opts.bind_host;
    cfg.gateway_port = opts.port;
    provider::DeepSeekProvider provider(api_key.empty() ? std::string("missing") : api_key, base,
                                        model);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    gateway::Server server(opts, provider, registry, store, channels, cfg);
    try {
        server.Run();
    } catch (const std::exception& e) {
        std::cerr << "gateway error: " << e.what() << '\n';
        curl_global_cleanup();
        return 1;
    }
    curl_global_cleanup();
    return 0;
}

int CmdStatus() {
    std::cout << nlohmann::json{
                     {"service", "openclaw-cpp"},
                     {"version", "0.2.0-dev"},
                     {"align_target", "OpenClaw 2026.10.5"},
                     {"modules",
                      nlohmann::json::array({"agent", "gateway", "tools", "session",
                                                 "provider/deepseek", "channels", "config"})},
                     {"todo", "channels, memory, plugins, mcp, cron, nodes, control-ui, ..."},
                 }
                     .dump(2)
              << '\n';
    return 0;
}

int CmdTools() {
    auto registry = openclaw::tools::MakeDefaultRegistry();
    for (const auto& n : registry.Names()) {
        std::cout << n << '\n';
    }
    return 0;
}


int CmdChannels() {
    auto ch = openclaw::channels::MakeDefaultChannels();
    std::cout << ch.ListJson().dump(2) << '\n';
    return 0;
}

int CmdConfig() {
    auto cfg = openclaw::config::LoadOrDefault(openclaw::config::DefaultPath());
    std::cout << cfg.ToJson().dump(2) << '\n';
    return 0;
}

int CmdMethods() {
    std::cout << nlohmann::json{
                     {"methods", openclaw::gateway::AdvertisedMethods()},
                     {"events", openclaw::gateway::AdvertisedEvents()},
                 }
                     .dump(2)
              << '\n';
    return 0;
}


int CmdMemory(int argc, char** argv, int argi) {
    openclaw::memory::Store store(
        std::filesystem::path(openclaw::EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")));
    if (argi >= argc) {
        std::cout << store.Dump().dump(2) << '\n';
        return 0;
    }
    const std::string sub = argv[argi++];
    if (sub == "set" && argi + 1 < argc) {
        store.Set(argv[argi], argv[argi + 1]);
        std::cout << "{\"ok\":true}\n";
        return 0;
    }
    if (sub == "get" && argi < argc) {
        auto v = store.Get(argv[argi]);
        if (!v) {
            std::cout << "{\"found\":false}\n";
            return 1;
        }
        std::cout << nlohmann::json{{"found", true}, {"value", *v}}.dump() << '\n';
        return 0;
    }
    std::cerr << "usage: openclaw-cpp memory [set KEY VAL | get KEY]\n";
    return 1;
}

int CmdCron() {
    // demo empty scheduler listing — persist later
    openclaw::cron::Scheduler sch;
    std::cout << sch.ListJson().dump(2) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            PrintUsage();
            return 1;
        }
        const std::string cmd = argv[1];
        if (cmd == "self-test") {
            return CmdSelfTest();
        }
        if (cmd == "status") {
            return CmdStatus();
        }
        if (cmd == "tools") {
            return CmdTools();
        }
        if (cmd == "methods") {
            return CmdMethods();
        }
        if (cmd == "memory") {
            return CmdMemory(argc, argv, 2);
        }
        if (cmd == "cron") {
            return CmdCron();
        }
        if (cmd == "config") {
            return CmdConfig();
        }
        if (cmd == "channels") {
            return CmdChannels();
        }
        if (cmd == "gateway") {
            return CmdGateway(argc, argv, 2);
        }
        if (cmd == "chat") {
            return CmdChat(argc, argv, 2);
        }
        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            PrintUsage();
            return 0;
        }
        // shorthand: openclaw-cpp <message>
        return CmdChat(argc, argv, 1);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
