#include "openclaw/agent/loop.hpp"
#include "openclaw/auth/gateway_auth.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/mcp/client.hpp"
#include "openclaw/config/validate.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/media/store.hpp"
#include "openclaw/nodes/registry.hpp"
#include "openclaw/events/bus.hpp"
#include "openclaw/mcp/bridge.hpp"
#include "openclaw/secrets/store.hpp"
#include "openclaw/approvals/store.hpp"
#include "openclaw/channels/adapter.hpp"
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
#include "openclaw/acp/session.hpp"
#include "openclaw/diagnostics/doctor.hpp"
#include "openclaw/persist/store.hpp"
#include "openclaw/provider/openai_compat.hpp"
#include "openclaw/provider/router.hpp"
#include "openclaw/agent/files.hpp"
#include "openclaw/logging/tail.hpp"
#include "openclaw/context/assemble.hpp"
#include "openclaw/mcp/stdio.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kVersion = "0.9.0-dev";

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
        << "  chat [-s SESSION] <msg> | gateway [--port N] | cron-daemon | mcp-mock-server | self-test\n"
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

    // webhook adapter
    {
        auto hub = channels::AdapterHub::MakeDefault();
        auto* wh = dynamic_cast<channels::WebhookAdapter*>(hub.Get("webhook"));
        if (!wh) { std::cerr << "FAIL webhook adapter\n"; ++failures; }
        else {
            auto out = wh->HandleInbound({{"text", "hi"}, {"sender", "t"}},
                                         [](const channels::InboundMessage& m) { return std::string("got:") + m.text; });
            if (!out.value("ok", false) || out.value("reply", "") != "got:hi") {
                std::cerr << "FAIL webhook inbound " << out.dump() << "\n"; ++failures;
            } else std::cerr << "PASS webhook inbound\n";
        }
        if (!hub.Get("telegram") || hub.Get("telegram")->Enabled()) {
            std::cerr << "FAIL telegram stub\n"; ++failures;
        } else std::cerr << "PASS im stubs\n";
    }

    // MCP mock client
    {
        std::string last_write;
        std::string next_read;
        mcp::Client client(
            [&](const std::string& line) { next_read = mcp::Client::MockServerHandle(nlohmann::json::parse(line)).dump(); },
            [&]() { return next_read; });
        if (!client.Initialize()) { std::cerr << "FAIL mcp init\n"; ++failures; }
        else {
            auto tools = client.ListTools();
            if (tools.empty() || tools[0].name != "mock_echo") {
                std::cerr << "FAIL mcp tools\n"; ++failures;
            } else {
                auto call = client.CallTool("mock_echo", {{"x", 1}});
                if (!call.contains("content")) { std::cerr << "FAIL mcp call\n"; ++failures; }
                else std::cerr << "PASS mcp client\n";
            }
        }
    }

    // approvals + secrets
    {
        approvals::Store ap;
        auto id = ap.Create("exec", "rm -rf /");
        if (!ap.Resolve(id, false) || ap.List("rejected").size() != 1) {
            std::cerr << "FAIL approvals\n"; ++failures;
        } else std::cerr << "PASS approvals\n";
        auto sp = std::filesystem::temp_directory_path() / "oc-sec-test.json";
        secrets::Store sec(sp);
        sec.Set("k", "v");
        if (!sec.Get("k") || *sec.Get("k") != "v" || sec.ListJson().empty()) {
            std::cerr << "FAIL secrets\n"; ++failures;
        } else std::cerr << "PASS secrets\n";
        std::filesystem::remove(sp);
    }



    // MCP bridge into tools
    {
        approvals::Store ap;
        tools::RegistryOptions o; o.approvals = &ap;
        auto reg = tools::MakeDefaultRegistry(o);
        auto cli = mcp::MakeMockClient();
        auto n = mcp::RegisterMcpTools(reg, cli);
        if (n < 1 || !reg.Has("mcp.mock_echo")) { std::cerr << "FAIL mcp bridge\n"; ++failures; }
        else {
            auto out = reg.Invoke("mcp.mock_echo", {{"hi", true}});
            if (out.find("content") == std::string::npos && out.find("echo") == std::string::npos) {
                std::cerr << "FAIL mcp bridge invoke " << out << "\n"; ++failures;
            } else std::cerr << "PASS mcp bridge\n";
        }
    }

    // shell approval gate
    {
        approvals::Store ap;
        tools::RegistryOptions o; o.approvals = &ap; o.shell_requires_approval = true;
        auto reg = tools::MakeDefaultRegistry(o);
        auto pending = nlohmann::json::parse(reg.Invoke("shell", {{"command", "echo hi"}}));
        if (!pending.value("needs_approval", false)) { std::cerr << "FAIL shell gate\n"; ++failures; }
        else {
            auto id = pending.value("approval_id", "");
            ap.Resolve(id, true);
            auto done = nlohmann::json::parse(reg.Invoke("shell", {{"command", "echo hi"}, {"approval_id", id}}));
            if (!done.value("ok", false) || !done.value("mocked", false)) {
                std::cerr << "FAIL shell approved " << done.dump() << "\n"; ++failures;
            } else std::cerr << "PASS shell approval\n";
        }
    }

    // event bus
    {
        events::Bus bus;
        int hits = 0;
        bus.Subscribe([&](const std::string& ev, const nlohmann::json&) { if (ev == "chat") ++hits; });
        bus.Emit("chat", {{"x", 1}});
        if (hits != 1 || bus.Recent(1).empty()) { std::cerr << "FAIL event bus\n"; ++failures; }
        else std::cerr << "PASS event bus\n";
    }

    // outbound telegram/feishu
    {
        auto hub = channels::AdapterHub::MakeDefault();
        auto* tg = dynamic_cast<channels::OutboundRecordAdapter*>(hub.Get("telegram"));
        auto* fs = dynamic_cast<channels::OutboundRecordAdapter*>(hub.Get("feishu"));
        if (!tg || !fs) { std::cerr << "FAIL outbound adapters\n"; ++failures; }
        else {
            tg->SetEnabled(true);
            auto r = tg->SendText("123", "hello-tg");
            if (!r.value("recorded", false) || tg->OutboundLog().empty()) {
                std::cerr << "FAIL telegram outbound\n"; ++failures;
            } else std::cerr << "PASS telegram/feishu outbound record\n";
        }
    }

    // config validate
    {
        auto cfg = config::LoadOrDefault(config::DefaultPath());
        auto v = config::ValidateLightJson(cfg);
        if (!v.value("ok", false)) { std::cerr << "FAIL config validate " << v.dump() << "\n"; ++failures; }
        else std::cerr << "PASS config validate\n";
    }


    // context assemble + compact
    {
        std::vector<agent::Message> hist;
        hist.push_back({.role = "user", .content = "u1"});
        hist.push_back({.role = "assistant", .content = "a1"});
        hist.push_back({.role = "tool", .content = std::string(5000, 'x'), .tool_call_id = "t1"});
        hist.push_back({.role = "assistant", .content = "a2"});
        context::AssembleOptions o;
        o.system_prompt = "sys";
        o.keep_recent = 2;
        o.max_tool_result_chars = 100;
        auto assembled = context::Assemble(hist, o);
        bool has_sys = !assembled.empty() && assembled.front().role == "system";
        auto compacted = context::Compact(hist, 2);
        if (!has_sys || compacted.size() > 3) {
            std::cerr << "FAIL context assemble/compact\n"; ++failures;
        } else std::cerr << "PASS context assemble/compact\n";
    }

    // event bus session filter
    {
        events::Bus bus;
        int hits = 0;
        events::SubFilter f; f.session_key = "s1";
        bus.Subscribe([&](const std::string&, const nlohmann::json&) { ++hits; }, f);
        bus.Emit("chat", {{"sessionKey", "s2"}});
        bus.Emit("chat", {{"sessionKey", "s1"}});
        if (hits != 1) { std::cerr << "FAIL bus filter hits=" << hits << "\n"; ++failures; }
        else std::cerr << "PASS bus session filter\n";
    }

    // MCP stdio child (spawn this binary as mcp-mock-server)
    {
        const char* self = "/workspace/openclaw-cpp/build/build/Release/openclaw-cpp";
        if (!std::filesystem::exists(self)) {
            std::cerr << "SKIP mcp stdio (binary path)\n";
        } else {
            try {
                auto cli = mcp::MakeStdioClient({self, "mcp-mock-server"});
                if (!cli->Initialize()) { std::cerr << "FAIL mcp stdio init\n"; ++failures; }
                else {
                    auto tools = cli->ListTools();
                    if (tools.empty()) { std::cerr << "FAIL mcp stdio list\n"; ++failures; }
                    else {
                        auto call = cli->CallTool(tools[0].name, {{"ping", 1}});
                        if (!call.contains("content")) { std::cerr << "FAIL mcp stdio call\n"; ++failures; }
                        else std::cerr << "PASS mcp stdio child\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "FAIL mcp stdio " << e.what() << "\n"; ++failures;
            }
        }
    }

    // agents files
    {
        auto tmp = std::filesystem::temp_directory_path() / "oc-agent-files";
        agent::FileStore fs(tmp);
        fs.Set("main", "SOUL.md", "hello");
        auto v = fs.Get("main", "SOUL.md");
        if (!v || *v != "hello") { std::cerr << "FAIL agent files\n"; ++failures; }
        else std::cerr << "PASS agent files\n";
    }

    // logs tail
    {
        logging::GlobalLog().Append("info", "self-test");
        if (logging::GlobalLog().Tail(1).empty()) { std::cerr << "FAIL logs\n"; ++failures; }
        else std::cerr << "PASS logs tail\n";
    }


    // provider router
    {
        provider::Router r;
        auto mock = std::make_shared<provider::MockProvider>();
        agent::CompletionResult mr;
        mr.ok = true; mr.finish_reason = "stop";
        mr.message.role = "assistant"; mr.message.content = "routed";
        mock->Enqueue(mr);
        r.Add("mock", mock);
        r.SetDefault("mock");
        auto out = r.Complete({{.role = "user", .content = "hi"}}, nlohmann::json::array());
        if (!out.ok || out.message.content != "routed") {
            std::cerr << "FAIL router\n"; ++failures;
        } else std::cerr << "PASS provider router\n";
    }

    // loop detection identical tools
    {
        provider::MockProvider mock;
        tools::Registry reg = tools::MakeDefaultRegistry();
        // two identical tool_calls turns then would break on 4th — feed 4 identical
        for (int i = 0; i < 4; ++i) {
            agent::CompletionResult first;
            first.ok = true; first.finish_reason = "tool_calls";
            first.message.role = "assistant";
            agent::ToolCall tc; tc.id = "c" + std::to_string(i); tc.name = "echo";
            tc.arguments_json = "{\"text\":\"same\"}";
            first.tool_calls.push_back(tc);
            first.message.tool_calls = nlohmann::json::array({
                {{"id", tc.id}, {"type", "function"},
                 {"function", {{"name", "echo"}, {"arguments", tc.arguments_json}}}},
            });
            mock.Enqueue(first);
        }
        auto result = agent::RunAgentLoop(mock, reg, {{.role = "user", .content = "x"}},
                                          {.max_turns = 8, .identical_tool_warn_after = 2, .identical_tool_break_after = 3});
        if (result.ok || result.error.find("loop detection") == std::string::npos) {
            std::cerr << "FAIL loop detect ok=" << result.ok << " err=" << result.error << "\n";
            ++failures;
        } else if (result.warnings.empty()) {
            std::cerr << "FAIL loop warn missing\n"; ++failures;
        } else std::cerr << "PASS loop detection\n";
    }

    // persist round-trip
    {
        auto dir = std::filesystem::temp_directory_path() / "oc-persist-test";
        std::filesystem::remove_all(dir);
        persist::StateStore st(dir);
        cron::Scheduler sch;
        sch.Add({.id = "j1", .cron_expr = "0 * * * *", .payload = "p", .enabled = true});
        st.SaveCron(sch);
        cron::Scheduler sch2;
        st.LoadCron(sch2);
        if (sch2.List().size() != 1 || sch2.List()[0].id != "j1") {
            std::cerr << "FAIL persist cron\n"; ++failures;
        } else std::cerr << "PASS persist state\n";
        std::filesystem::remove_all(dir);
    }

    // doctor
    {
        auto doc = diagnostics::RunDoctorJson({{"version", "0.9.0-dev"},
                                              {"methods_implemented", 10},
                                              {"providers", nlohmann::json::array({"mock"})},
                                              {"deepseek_key_set", false},
                                              {"memory_keys", 0},
                                              {"cron_jobs", 0}});
        if (!doc.contains("checks")) { std::cerr << "FAIL doctor\n"; ++failures; }
        else std::cerr << "PASS doctor diagnostics\n";
    }


    // secrets encryption
    {
        auto sp = std::filesystem::temp_directory_path() / "oc-sec-enc.json";
        std::filesystem::remove(sp);
        secrets::Store sec(sp, "test-passphrase");
        sec.Set("api", "super-secret");
        auto v = sec.Get("api");
        // file must not contain plaintext
        std::string file = (std::ifstream(sp) >> std::noskipws, std::string{std::istreambuf_iterator<char>(std::ifstream(sp).rdbuf()), {}});
        // re-read properly
        {
            std::ifstream in(sp); std::string s((std::istreambuf_iterator<char>(in)), {});
            file = s;
        }
        if (!v || *v != "super-secret" || file.find("super-secret") != std::string::npos || !sec.Encrypted()) {
            std::cerr << "FAIL secrets encrypt\n"; ++failures;
        } else std::cerr << "PASS secrets encrypt\n";
        std::filesystem::remove(sp);
    }

    // providers hot-switch (router)
    {
        provider::Router r;
        auto m1 = std::make_shared<provider::MockProvider>();
        auto m2 = std::make_shared<provider::MockProvider>();
        agent::CompletionResult a; a.ok=true; a.finish_reason="stop"; a.message.role="assistant"; a.message.content="p1";
        agent::CompletionResult b; b.ok=true; b.finish_reason="stop"; b.message.role="assistant"; b.message.content="p2";
        m1->Enqueue(a); m2->Enqueue(b);
        r.Add("p1", m1); r.Add("p2", m2); r.SetDefault("p1");
        r.SetDefault("p2");
        auto out = r.Complete({{.role="user",.content="x"}}, nlohmann::json::array());
        if (!out.ok || out.message.content != "p2") { std::cerr << "FAIL provider switch\n"; ++failures; }
        else std::cerr << "PASS provider hot-switch\n";
    }

    // implemented count >= 150
    if (gateway::ImplementedMethods().size() < 150) {
        std::cerr << "FAIL implemented count " << gateway::ImplementedMethods().size() << "\n"; ++failures;
    } else std::cerr << "PASS implemented count (" << gateway::ImplementedMethods().size() << ")\n";


    // ACP skeleton
    {
        acp::Registry acp;
        auto id = acp.Create("codex");
        acp.AppendPrompt(id, "hi");
        auto st = acp.StatusJson();
        if (!st.value("skeleton", false) || acp.ListJson().empty()) {
            std::cerr << "FAIL acp skeleton\n"; ++failures;
        } else std::cerr << "PASS acp skeleton\n";
    }

    // outbound record without tokens (graceful)
    {
        auto hub = channels::AdapterHub::MakeDefault();
        auto* tg = dynamic_cast<channels::OutboundRecordAdapter*>(hub.Get("telegram"));
        auto* dc = dynamic_cast<channels::OutboundRecordAdapter*>(hub.Get("discord"));
        if (!tg || !dc) { std::cerr << "FAIL outbound adapters\n"; ++failures; }
        else {
            auto r = tg->SendText("1", "no-token-test");
            if (!r.value("recorded", false) || r.value("mode", "") != "record_only") {
                std::cerr << "FAIL telegram no-token " << r.dump() << "\n"; ++failures;
            } else std::cerr << "PASS telegram/discord outbound graceful\n";
        }
    }

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
    approvals::Store approvals;
    tools::RegistryOptions tropts;
    tropts.approvals = &approvals;
    tropts.enable_shell = true;
    tropts.shell_requires_approval = true;
    auto tools = tools::MakeDefaultRegistry(tropts);
    auto mcp_client = mcp::MakeMockClient();
    auto mcp_n = mcp::RegisterMcpTools(tools, mcp_client);
    std::cerr << "[gateway] bridged " << mcp_n << " mcp tools\n";

    // Multi-provider router (DeepSeek + OpenAI-compat + mock)
    provider::Router router;
    const std::string ds_key = api_key;
    const std::string ds_base = EnvOr("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    const std::string ds_model = EnvOr("DEEPSEEK_MODEL", "deepseek-chat");
    router.Add("deepseek", std::make_shared<provider::OpenAiCompatProvider>("deepseek", ds_key, ds_base, ds_model));
    const std::string oai_key = EnvOr("OPENAI_API_KEY", ds_key);
    const std::string oai_base = EnvOr("OPENAI_BASE_URL", "https://api.openai.com");
    const std::string oai_model = EnvOr("OPENAI_MODEL", "gpt-4o-mini");
    router.Add("openai", std::make_shared<provider::OpenAiCompatProvider>("openai", oai_key, oai_base, oai_model));
    auto mock_prov = std::make_shared<provider::MockProvider>();
    // seed a trivial stop response for mock route health
    {
        agent::CompletionResult mr;
        mr.ok = true;
        mr.finish_reason = "stop";
        mr.message.role = "assistant";
        mr.message.content = "mock-ok";
        mock_prov->Enqueue(mr);
    }
    router.Add("mock", mock_prov);
    router.SetDefault(EnvOr("OPENCLAW_DEFAULT_PROVIDER", "deepseek"));

    auto channels = channels::MakeDefaultChannels();
    auto adapters = channels::AdapterHub::MakeDefault();
    auto skills = LoadSkills();
    plugins::Registry plugins;
    plugins.Scan(RepoRoot() / "plugins");
    cron::Scheduler cron;
    auth::PairingStore pairing;
    session::Store sessions(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    memory::Store memory(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    secrets::Store secrets(std::filesystem::path(EnvOr("HOME", "/tmp")) / ".openclaw-cpp" / "secrets.json",
                             EnvOr("OPENCLAW_SECRETS_PASSPHRASE", ""));
    auto nodes = nodes::Registry::MakeDefault();
    media::Store media(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "media");
    events::Bus bus;
    agent::FileStore agent_files(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "agents");
    auto& logs = logging::GlobalLog();
    logs.Append("info", "gateway starting");

    const auto data_dir = std::filesystem::path(EnvOr("OPENCLAW_DATA_DIR",
        (std::filesystem::path(EnvOr("HOME", "/tmp")) / ".openclaw-cpp" / "data").string()));
    persist::StateStore persist(data_dir);
    acp::Registry acp_reg;
    persist.LoadCron(cron);
    persist.LoadPairing(pairing);
    persist.LoadNodes(nodes);
    if (pairing.List().empty()) {
        pairing.Add({.id = "pair-demo", .channel = "cli", .code = "ABC123", .sender = "local"});
    }

    if (const char* mcp_cmd = std::getenv("OPENCLAW_MCP_COMMAND"); mcp_cmd && *mcp_cmd) {
        try {
            std::vector<std::string> argv;
            std::istringstream iss(mcp_cmd);
            std::string tok;
            while (iss >> tok) argv.push_back(tok);
            auto stdio_cli = mcp::MakeStdioClient(argv);
            auto n = mcp::RegisterMcpTools(tools, stdio_cli, "mcp.stdio.");
            std::cerr << "[gateway] stdio mcp bridged " << n << " tools\n";
        } catch (const std::exception& e) {
            std::cerr << "[gateway] stdio mcp failed: " << e.what() << "\n";
        }
    }
    auto cfg = config::LoadOrDefault(config::DefaultPath());
    cfg.gateway_host = opts.bind_host;
    cfg.gateway_port = opts.port;

    // Keep DeepSeekProvider for streaming path
    provider::DeepSeekProvider deepseek(ds_key, ds_base, ds_model);

    gateway::ServerDeps deps{
        .provider = router,
        .router = &router,
        .deepseek = &deepseek,
        .tools = tools,
        .sessions = sessions,
        .channels = channels,
        .adapters = adapters,
        .skills = skills,
        .plugins = plugins,
        .cron = cron,
        .pairing = pairing,
        .approvals = approvals,
        .secrets = secrets,
        .memory = memory,
        .nodes = nodes,
        .media = media,
        .bus = bus,
        .agent_files = agent_files,
        .logs = logs,
        .persist = &persist,
        .acp = &acp_reg,
        .config = cfg,
        .auth = auth::FromEnv(),
    };


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


int CmdMcpMockServer() {
    // Line-delimited JSON-RPC MCP mock on stdio (for MakeStdioClient self-test).
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            auto req = nlohmann::json::parse(line);
            auto resp = openclaw::mcp::Client::MockServerHandle(req);
            std::cout << resp.dump() << std::endl;
            std::cout.flush();
        } catch (const std::exception& e) {
            std::cout << nlohmann::json{{"jsonrpc", "2.0"},
                                        {"id", nullptr},
                                        {"error", {{"message", e.what()}}}}.dump()
                      << std::endl;
        }
    }
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
            openclaw::approvals::Store ap;
            openclaw::tools::RegistryOptions o; o.approvals = &ap;
            auto reg = openclaw::tools::MakeDefaultRegistry(o);
            openclaw::mcp::RegisterMcpTools(reg, openclaw::mcp::MakeMockClient());
            for (auto& n : reg.Names()) std::cout << n << '\n';
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
        if (cmd == "mcp-mock-server") return CmdMcpMockServer();
        if (cmd == "gateway") return CmdGateway(argc, argv, 2);
        if (cmd == "chat") return CmdChat(argc, argv, 2);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") { PrintUsage(); return 0; }
        return CmdChat(argc, argv, 1);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
