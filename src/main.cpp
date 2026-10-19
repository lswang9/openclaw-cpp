#include "openclaw/agent/loop.hpp"
#include "openclaw/auth/gateway_auth.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/cron/scheduler.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/gateway/server.hpp"
#include "openclaw/memory/store.hpp"
#include "openclaw/plugins/registry.hpp"
#include "openclaw/provider/deepseek.hpp"
#include "openclaw/provider/mock.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/skills/loader.hpp"
#include "openclaw/tools/registry.hpp"
#include "openclaw/util.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kVersion = "0.3.0-dev";

std::string JoinArgs(int argc, char** argv, int start) {
    std::ostringstream oss;
    for (int i = start; i < argc; ++i) {
        if (i > start) oss << ' ';
        oss << argv[i];
    }
    return oss.str();
}

std::string ReadStdin() {
    std::ostringstream oss;
    oss << std::cin.rdbuf();
    std::string s = oss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void PrintUsage() {
    std::cerr
        << "openclaw-cpp " << kVersion << " — align OpenClaw 2026.10.5\n"
        << "  chat [-s SESSION] <msg> | gateway [--port N] | cron-daemon | self-test\n"
        << "  status | tools | channels | config | methods | skills | memory | cron\n";
}

std::filesystem::path RepoRoot() {
    // Prefer binary-adjacent / workspace path for skills/examples
    return std::filesystem::path("/workspace/openclaw-cpp");
}

openclaw::skills::Library LoadSkills() {
    openclaw::skills::Library lib;
    std::vector<std::filesystem::path> roots = {
        RepoRoot() / "skills" / "examples",
        std::filesystem::path(openclaw::EnvOr("OPENCLAW_SKILLS_DIR", "")),
        std::filesystem::path(openclaw::EnvOr("HOME", "/tmp")) / ".openclaw-cpp" / "skills",
    };
    // drop empty
    std::vector<std::filesystem::path> cleaned;
    for (auto& r : roots) {
        if (!r.empty()) cleaned.push_back(r);
    }
    lib.Scan(cleaned);
    return lib;
}

int CmdSelfTest() {
    using namespace openclaw;
    int failures = 0;

    auto registry = tools::MakeDefaultRegistry();
    if (registry.Invoke("echo", {{"text", "ping"}}).find("ping") == std::string::npos) {
        std::cerr << "FAIL echo\n"; ++failures;
    } else std::cerr << "PASS echo\n";

    provider::MockProvider mock;
    {
        agent::CompletionResult first;
        first.ok = true; first.finish_reason = "tool_calls";
        first.message.role = "assistant";
        agent::ToolCall tc; tc.id = "call_1"; tc.name = "echo"; tc.arguments_json = "{\"text\":\"hello-loop\"}";
        first.tool_calls.push_back(tc);
        first.message.tool_calls = nlohmann::json::array({
            {{"id", "call_1"}, {"type", "function"},
             {"function", {{"name", "echo"}, {"arguments", "{\"text\":\"hello-loop\"}"}}}},
        });
        mock.Enqueue(first);
        agent::CompletionResult second;
        second.ok = true; second.finish_reason = "stop";
        second.message.role = "assistant"; second.message.content = "loop-ok";
        mock.Enqueue(second);
    }
    auto result = agent::RunAgentLoop(mock, registry, {agent::Message{.role = "user", .content = "x"}}, {});
    if (!result.ok || result.turns != 2) { std::cerr << "FAIL agent loop\n"; ++failures; }
    else std::cerr << "PASS agent loop\n";

    auto tmp = std::filesystem::temp_directory_path() / "openclaw-cpp-st";
    session::Store store(tmp);
    store.Save("t1", {agent::Message{.role = "user", .content = "hi"}});
    if (store.Load("t1").size() != 1) { std::cerr << "FAIL session\n"; ++failures; }
    else std::cerr << "PASS session\n";
    store.Remove("t1");

    if (gateway::AdvertisedMethodCount() < 100) {
        std::cerr << "FAIL methods catalog too small\n"; ++failures;
    } else std::cerr << "PASS methods catalog (" << gateway::AdvertisedMethodCount() << ")\n";

    if (gateway::ImplementedMethods().size() < 10) {
        std::cerr << "FAIL implemented methods\n"; ++failures;
    } else std::cerr << "PASS implemented methods (" << gateway::ImplementedMethods().size() << ")\n";

    auto skills = LoadSkills();
    if (skills.List().empty()) { std::cerr << "FAIL skills loader (expected hello-skill)\n"; ++failures; }
    else std::cerr << "PASS skills loader (" << skills.List().size() << ")\n";

    // cron match
    std::tm tm{}; tm.tm_min = 0; tm.tm_hour = 12; tm.tm_mday = 1; tm.tm_mon = 0; tm.tm_wday = 3;
    if (!cron::MatchesCron("0 12 1 1 *", tm)) { std::cerr << "FAIL cron match\n"; ++failures; }
    else std::cerr << "PASS cron match\n";
    cron::Scheduler sch;
    sch.Add({.id = "t", .cron_expr = "* * * * *", .payload = "x", .enabled = true});
    auto now = std::chrono::system_clock::now();
    auto fired = sch.Tick(now);
    auto fired2 = sch.Tick(now);
    if (fired != 1 || fired2 != 0) { std::cerr << "FAIL cron tick dedupe fired=" << fired << " " << fired2 << "\n"; ++failures; }
    else std::cerr << "PASS cron tick\n";

    // auth
    auth::AuthConfig ac; ac.gateway_token = "secret"; ac.require_auth = true;
    if (!auth::CheckHttpAuth(ac, "Bearer secret", "").has_value()) std::cerr << "PASS auth allow\n";
    else { std::cerr << "FAIL auth allow\n"; ++failures; }
    if (auth::CheckHttpAuth(ac, "Bearer wrong", "").has_value()) std::cerr << "PASS auth deny\n";
    else { std::cerr << "FAIL auth deny\n"; ++failures; }

    if (channels::MakeDefaultChannels().List().size() < 3) { std::cerr << "FAIL channels\n"; ++failures; }
    else std::cerr << "PASS channels\n";

    if (failures == 0) { std::cerr << "self-test: ALL PASS\n"; return 0; }
    std::cerr << "self-test: " << failures << " failure(s)\n";
    return 1;
}

int CmdChat(int argc, char** argv, int argi) {
    using namespace openclaw;
    std::string session_id;
    while (argi < argc) {
        std::string a = argv[argi];
        if ((a == "-s" || a == "--session") && argi + 1 < argc) { session_id = argv[++argi]; ++argi; continue; }
        break;
    }
    std::string user_message = JoinArgs(argc, argv, argi);
    if (user_message.empty()) user_message = ReadStdin();
    if (user_message.empty()) { std::cerr << "empty message\n"; return 1; }
    std::string api_key = EnvOr("DEEPSEEK_API_KEY", "");
    if (api_key.empty()) { std::cerr << "set DEEPSEEK_API_KEY\n"; return 1; }
    auto registry = tools::MakeDefaultRegistry();
    session::Store store(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    provider::DeepSeekProvider provider(api_key, EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
                                        EnvOr("DEEPSEEK_MODEL", "deepseek-chat"));
    auto messages = store.Load(session_id.empty() ? "_" : session_id);
    messages.push_back({.role = "user", .content = user_message});
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto result = agent::RunAgentLoop(provider, registry, messages, {});
    curl_global_cleanup();
    if (!session_id.empty() && result.ok) store.Save(session_id, result.messages);
    if (!result.ok) {
        std::cerr << "error: " << result.error << '\n';
        return result.error.find("402") != std::string::npos ? 2 : 1;
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
        std::string a = argv[argi++];
        if ((a == "--port" || a == "-p") && argi < argc) opts.port = static_cast<std::uint16_t>(std::stoi(argv[argi++]));
        else if (a == "--host" && argi < argc) opts.bind_host = argv[argi++];
        else if (a == "--verbose" || a == "-v") opts.verbose = true;
    }
    std::string api_key = EnvOr("DEEPSEEK_API_KEY", "missing");
    auto tools = tools::MakeDefaultRegistry();
    auto channels = channels::MakeDefaultChannels();
    auto skills = LoadSkills();
    plugins::Registry plugins;
    plugins.Scan(RepoRoot() / "plugins");
    cron::Scheduler cron;
    auth::PairingStore pairing;
    // demo pending pair
    pairing.Add({.id = "pair-demo", .channel = "cli", .code = "ABC123", .sender = "local"});
    session::Store sessions(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    auto cfg = config::LoadOrDefault(config::DefaultPath());
    cfg.gateway_host = opts.bind_host;
    cfg.gateway_port = opts.port;
    provider::DeepSeekProvider deepseek(api_key, EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
                                        EnvOr("DEEPSEEK_MODEL", "deepseek-chat"));
    gateway::ServerDeps deps{
        .provider = deepseek,
        .deepseek = &deepseek,
        .tools = tools,
        .sessions = sessions,
        .channels = channels,
        .skills = skills,
        .plugins = plugins,
        .cron = cron,
        .pairing = pairing,
        .config = cfg,
        .auth = auth::FromEnv(),
    };
    curl_global_init(CURL_GLOBAL_DEFAULT);
    gateway::Server server(opts, deps);
    try { server.Run(); }
    catch (const std::exception& e) {
        std::cerr << "gateway error: " << e.what() << '\n';
        curl_global_cleanup();
        return 1;
    }
    curl_global_cleanup();
    return 0;
}

int CmdCronDaemon() {
    using namespace openclaw;
    cron::Scheduler sch;
    sch.Add({.id = "heartbeat", .cron_expr = "*/1 * * * *", .payload = "tick", .enabled = true});
    std::atomic<bool> running{true};
    std::cerr << "[cron-daemon] running (Ctrl-C not hooked; kill process to stop)\n";
    // run a few seconds in self-contained mode if OPENCLAW_CRON_DEMO_SECONDS set
    const int demo = std::stoi(EnvOr("OPENCLAW_CRON_DEMO_SECONDS", "0"));
    if (demo > 0) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(demo);
        while (std::chrono::steady_clock::now() < end) {
            sch.Tick(std::chrono::system_clock::now(), [](const cron::Job& j) {
                std::cerr << "[cron] fire " << j.id << " payload=" << j.payload
                          << " count=" << j.run_count << '\n';
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::cout << sch.ListJson().dump(2) << '\n';
        return 0;
    }
    sch.RunDaemon(running, [](const cron::Job& j) {
        std::cerr << "[cron] fire " << j.id << '\n';
    });
    return 0;
}

int CmdStatus() {
    std::cout << nlohmann::json{
        {"service", "openclaw-cpp"}, {"version", kVersion}, {"align_target", "OpenClaw 2026.10.5"},
        {"methods_advertised", openclaw::gateway::AdvertisedMethodCount()},
        {"methods_implemented", openclaw::gateway::ImplementedMethods().size()},
        {"events_advertised", openclaw::gateway::AdvertisedEventCount()},
    }.dump(2) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { PrintUsage(); return 1; }
        std::string cmd = argv[1];
        if (cmd == "self-test") return CmdSelfTest();
        if (cmd == "status") return CmdStatus();
        if (cmd == "tools") {
            for (auto& n : openclaw::tools::MakeDefaultRegistry().Names()) std::cout << n << '\n';
            return 0;
        }
        if (cmd == "channels") {
            std::cout << openclaw::channels::MakeDefaultChannels().ListJson().dump(2) << '\n';
            return 0;
        }
        if (cmd == "config") {
            std::cout << openclaw::config::LoadOrDefault(openclaw::config::DefaultPath()).ToJson().dump(2) << '\n';
            return 0;
        }
        if (cmd == "methods") {
            std::cout << nlohmann::json{
                {"implemented", openclaw::gateway::ImplementedMethods()},
                {"advertised_count", openclaw::gateway::AdvertisedMethodCount()},
                {"events_count", openclaw::gateway::AdvertisedEventCount()},
            }.dump(2) << '\n';
            return 0;
        }
        if (cmd == "skills") {
            std::cout << LoadSkills().StatusJson().dump(2) << '\n';
            return 0;
        }
        if (cmd == "memory") {
            openclaw::memory::Store store(openclaw::EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
            if (argc == 2) { std::cout << store.Dump().dump(2) << '\n'; return 0; }
            std::string sub = argv[2];
            if (sub == "set" && argc >= 5) { store.Set(argv[3], argv[4]); std::cout << "{\"ok\":true}\n"; return 0; }
            if (sub == "get" && argc >= 4) {
                auto v = store.Get(argv[3]);
                if (!v) { std::cout << "{\"found\":false}\n"; return 1; }
                std::cout << nlohmann::json{{"found", true}, {"value", *v}}.dump() << '\n';
                return 0;
            }
            return 1;
        }
        if (cmd == "cron") {
            openclaw::cron::Scheduler sch;
            std::cout << sch.ListJson().dump(2) << '\n';
            return 0;
        }
        if (cmd == "cron-daemon") return CmdCronDaemon();
        if (cmd == "gateway") return CmdGateway(argc, argv, 2);
        if (cmd == "chat") return CmdChat(argc, argv, 2);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") { PrintUsage(); return 0; }
        return CmdChat(argc, argv, 1);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
