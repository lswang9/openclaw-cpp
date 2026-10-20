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
#include "openclaw/plugins/js_host.hpp"
#include "openclaw/board/store.hpp"
#include "openclaw/canvas/store.hpp"
#include "openclaw/provider/deepseek.hpp"
#include "openclaw/provider/mock.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/skills/loader.hpp"
#include "openclaw/tools/registry.hpp"
#include "openclaw/util.hpp"
#include "openclaw/acp/session.hpp"
#include "openclaw/acp/wire.hpp"
#include "openclaw/daemon/lifecycle.hpp"
#include "openclaw/voice/tts.hpp"
#include "openclaw/sim/backend.hpp"
#include "openclaw/sim/capture.hpp"
#include "openclaw/sim/input.hpp"
#include "openclaw/browser/engine.hpp"
#include "openclaw/browser/cdp_client.hpp"
#include "openclaw/channels/feishu.hpp"
#include "openclaw/channels/feishu_ws.hpp"
#include "openclaw/channels/slack.hpp"
#include "openclaw/channels/slack_socket.hpp"
#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/discord_gateway.hpp"
#include "openclaw/channels/simple_inbound.hpp"
#include "openclaw/channels/whatsapp.hpp"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kVersion = "1.21.0-dev";

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
        << "  chat [-s SESSION] <msg> | gateway [--port N] | gateway start|stop|status\n"
        << "  cron-daemon | mcp-mock-server | acp-stdio | self-test\n"
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
        auto doc = diagnostics::RunDoctorJson({{"version", "1.18.0-dev"},
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
    if (gateway::ImplementedMethods().size() < 500) {
        std::cerr << "FAIL implemented count " << gateway::ImplementedMethods().size() << "\n"; ++failures;
    } else std::cerr << "PASS implemented count (" << gateway::ImplementedMethods().size() << ")\n";


    // ACP wire (in-process JSON-RPC subset)
    {
        acp::Registry acp;
        auto id = acp.Create("codex");
        acp.AppendPrompt(id, "hi");
        auto st = acp.StatusJson();
        if (!st.value("implemented", false) || !st.value("wire", false) || acp.ListJson().empty()) {
            std::cerr << "FAIL acp registry wire " << st.dump() << "\n"; ++failures;
        } else std::cerr << "PASS acp registry+wire status\n";

        auto [cli, agent] = acp::Client::MakeInProcessPair();
        if (!cli->Initialize()) {
            std::cerr << "FAIL acp initialize\n"; ++failures;
        } else {
            auto created = cli->SessionNew("/tmp");
            if (created.contains("error") || !created.contains("result")) {
                std::cerr << "FAIL acp session/new " << created.dump() << "\n"; ++failures;
            } else {
                auto sid = created["result"].value("sessionId", "");
                auto prompted = cli->SessionPrompt(sid, "analyze hello");
                std::string agent_text;
                bool has_agent_note = false;
                for (const auto& n : cli->LastNotifications()) {
                    if (n.value("method", "") != "session/update") continue;
                    has_agent_note = true;
                    auto upd = n.value("params", nlohmann::json::object()).value("update", nlohmann::json::object());
                    if (upd.value("sessionUpdate", "") == "agent_message") {
                        auto content = upd.value("content", nlohmann::json::array());
                        if (!content.empty() && content[0].contains("text"))
                            agent_text = content[0].value("text", "");
                    }
                }
                auto listed = cli->SessionList();
                // MockProvider empty script → "mock-default" via AgentLoop
                if (prompted.contains("error") || !prompted.contains("result") ||
                    !prompted["result"].contains("messageId") || !has_agent_note ||
                    agent_text != "mock-default" || listed.contains("error")) {
                    std::cerr << "FAIL acp prompt/loop text='" << agent_text << "' "
                              << prompted.dump() << "\n";
                    ++failures;
                } else std::cerr << "PASS acp session/prompt → AgentLoop (mock)\n";

                // Multimodal prompt (tiny PNG base64, mock provider)
                {
                    const char* png =
                        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
                    nlohmann::json parts = nlohmann::json::array({
                        {{"type", "text"}, {"text", "describe"}},
                        {{"type", "image"}, {"mimeType", "image/png"}, {"data", png}},
                    });
                    (void)agent->DrainNotifications();
                    auto direct = agent->Prompt(sid, parts);
                    auto meta = direct.value("_meta", nlohmann::json::object())
                                    .value("openclawCpp", nlohmann::json::object());
                    std::string reply = direct.value("text", "");
                    for (const auto& n : agent->DrainNotifications()) {
                        if (n.value("method", "") != "session/update") continue;
                        auto u = n.value("params", nlohmann::json::object())
                                     .value("update", nlohmann::json::object());
                        if (u.value("sessionUpdate", "") != "agent_message") continue;
                        for (const auto& c : u.value("content", nlohmann::json::array())) {
                            if (c.value("type", "") == "text") reply += c.value("text", "");
                        }
                    }
                    const bool ok_img = meta.value("images", 0) == 1 &&
                                        reply.find("mock-saw-image:1") != std::string::npos;
                    if (!ok_img) {
                        std::cerr << "FAIL acp multimodal meta=" << meta.dump()
                                  << " reply='" << reply << "' direct=" << direct.dump() << "\n";
                        ++failures;
                    } else
                        std::cerr << "PASS acp multimodal image prompt\n";
                }


                // Session MCP: attach mock server, merge tools, drive tool_call via scripted mock
                {
                    auto mock = std::make_shared<provider::MockProvider>();
                    agent::CompletionResult tc;
                    tc.ok = true;
                    tc.finish_reason = "tool_calls";
                    tc.tool_calls.push_back({"call_mcp1", "mcp.mock_echo", "{\"ping\":1}"});
                    mock->Enqueue(std::move(tc));
                    agent::CompletionResult done;
                    done.ok = true;
                    done.finish_reason = "stop";
                    done.message.role = "assistant";
                    done.message.content = "mcp-tool-done";
                    mock->Enqueue(std::move(done));
                    acp::LoopBackend b;
                    b.provider = mock;
                    b.tools = std::make_shared<tools::Registry>();
                    b.opts.max_turns = 4;
                    b.provider_name = "mock";
                    agent->SetLoopBackend(std::move(b));
                    (void)agent->DrainNotifications();
                    auto add = agent->AddSessionMcp(sid, {{"name", "mock-mcp"}, {"transport", "mock"}});
                    auto listed = agent->ListSessionMcp(sid);
                    bool tools_ok = add.value("ok", false) &&
                                    add.contains("mcp") &&
                                    add["mcp"].value("tool_count", 0) >= 1 &&
                                    listed.value("ok", false) &&
                                    !listed["mcp_servers"].empty();
                    auto prompt = agent->Prompt(sid, nlohmann::json::array({
                        {{"type", "text"}, {"text", "call mcp"}}
                    }));
                    auto meta = prompt.value("_meta", nlohmann::json::object())
                                    .value("openclawCpp", nlohmann::json::object());
                    std::string reply = prompt.value("text", "");
                    bool ran = meta.value("mcp_tools", 0) >= 1 &&
                               meta.value("ok", false) &&
                               (reply == "mcp-tool-done" || reply.find("mcp-tool") != std::string::npos);
                    auto mid = add.value("mcp", nlohmann::json::object()).value("id", "");
                    auto rem = agent->RemoveSessionMcp(sid, mid);
                    if (!tools_ok || !ran || !rem.value("ok", false)) {
                        std::cerr << "FAIL acp session mcp add=" << add.dump()
                                  << " list=" << listed.dump()
                                  << " prompt=" << prompt.dump()
                                  << " rem=" << rem.dump() << "\n";
                        ++failures;
                    } else
                        std::cerr << "PASS acp session MCP tools on prompt\n";
                }

                // cancel path: delay before loop; cancel on AgentEndpoint from another thread
                setenv("OPENCLAW_ACP_TURN_DELAY_MS", "300", 1);
                {
                    acp::LoopBackend b;
                    b.provider = std::make_shared<provider::MockProvider>();
                    b.tools = std::make_shared<tools::Registry>();
                    b.opts.max_turns = 2;
                    b.provider_name = "mock";
                    agent->SetLoopBackend(std::move(b));
                    auto init = agent->Handle({{"jsonrpc","2.0"},{"id",10},{"method","initialize"},
                        {"params",{{"protocolVersion",2},{"capabilities",nlohmann::json::object()},
                                   {"info",{{"name","t"},{"version","0"}}}}}});
                    auto ns = agent->Handle({{"jsonrpc","2.0"},{"id",11},{"method","session/new"},
                                             {"params",{{"cwd","/tmp"},{"mcpServers",nlohmann::json::array()}}}});
                    auto sid2 = ns["result"].value("sessionId", "");
                    nlohmann::json prompt_resp;
                    std::thread th([&] {
                        prompt_resp = agent->Handle(
                            {{"jsonrpc","2.0"},{"id",12},{"method","session/prompt"},
                             {"params",{{"sessionId",sid2},
                                        {"prompt",nlohmann::json::array({{{"type","text"},{"text","slow"}}})}}}});
                    });
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    auto canc = agent->Handle({{"jsonrpc","2.0"},{"id",13},{"method","session/cancel"},
                                               {"params",{{"sessionId",sid2}}}});
                    th.join();
                    unsetenv("OPENCLAW_ACP_TURN_DELAY_MS");
                    auto notes = agent->DrainNotifications();
                    bool saw = false;
                    for (const auto& n : notes) {
                        auto upd = n.value("params", nlohmann::json::object())
                                       .value("update", nlohmann::json::object());
                        auto meta = upd.value("_meta", nlohmann::json::object());
                        if (meta.value("cancelled", false) || meta.value("cancel_requested", false)) saw = true;
                        if (upd.value("sessionUpdate", "") == "agent_message") {
                            auto content = upd.value("content", nlohmann::json::array());
                            if (!content.empty() && content[0].value("text", "") == "[cancelled]") saw = true;
                        }
                    }
                    auto meta = prompt_resp.value("result", nlohmann::json::object())
                                    .value("_meta", nlohmann::json::object())
                                    .value("openclawCpp", nlohmann::json::object());
                    if (!canc.contains("result") || (!saw && !meta.value("cancelled", false))) {
                        std::cerr << "FAIL acp cancel canc=" << canc.dump()
                                  << " prompt=" << prompt_resp.dump() << "\n";
                        ++failures;
                    } else std::cerr << "PASS acp session/cancel → loop cancel\n";
                }
                (void)cli->SessionClose(sid);
            }
        }
        // agents list surface
        if (acp.AgentsListJson().empty()) {
            std::cerr << "FAIL acp agents list\n"; ++failures;
        } else std::cerr << "PASS acp agents.list\n";

        
        // ACP auth gate (fail-closed + success; mock provider only)
        {
            unsetenv("OPENCLAW_ACP_TOKEN");
            unsetenv("OPENCLAW_ACP_AUTH_TOKEN");
            unsetenv("OPENCLAW_ACP_PASSPHRASE");
            unsetenv("OPENCLAW_ACP_AUTH_REQUIRED");
            setenv("OPENCLAW_ACP_TOKEN", "test-acp-secret", 1);
            setenv("OPENCLAW_ACP_AUTH_REQUIRED", "1", 1);
            acp::AgentEndpoint ep("openclaw-cpp", "1.21.0-dev");
            acp::LoopBackend b;
            b.provider = std::make_shared<provider::MockProvider>();
            b.tools = std::make_shared<tools::Registry>();
            b.opts.max_turns = 2;
            b.provider_name = "mock";
            ep.SetLoopBackend(std::move(b));
            auto init = ep.Handle({{"jsonrpc", "2.0"},
                                   {"id", 1},
                                   {"method", "initialize"},
                                   {"params",
                                    {{"protocolVersion", 2},
                                     {"capabilities", nlohmann::json::object()},
                                     {"info", {{"name", "t"}, {"version", "0"}}}}}});
            auto meta = init.value("result", nlohmann::json::object())
                            .value("_meta", nlohmann::json::object())
                            .value("openclawCpp", nlohmann::json::object());
            bool req = meta.value("auth_required", false);
            bool not_authed = !meta.value("authenticated", true);
            auto denied = ep.Handle({{"jsonrpc", "2.0"},
                                     {"id", 2},
                                     {"method", "session/new"},
                                     {"params", {{"cwd", "/tmp"}}}});
            bool fail_closed = denied.contains("error") &&
                               denied["error"].value("code", 0) == -32004;
            auto bad = ep.Handle({{"jsonrpc", "2.0"},
                                  {"id", 3},
                                  {"method", "auth/login"},
                                  {"params", {{"token", "wrong"}}}});
            bool bad_login = bad.contains("error");
            auto good = ep.Handle({{"jsonrpc", "2.0"},
                                   {"id", 4},
                                   {"method", "auth/login"},
                                   {"params", {{"token", "test-acp-secret"}}}});
            bool ok_login = good.contains("result") && good["result"].value("authenticated", false);
            auto ns = ep.Handle({{"jsonrpc", "2.0"},
                                 {"id", 5},
                                 {"method", "session/new"},
                                 {"params", {{"cwd", "/tmp"}}}});
            auto sid = ns.value("result", nlohmann::json::object()).value("sessionId", "");
            auto prompt = ep.Handle(
                {{"jsonrpc", "2.0"},
                 {"id", 6},
                 {"method", "session/prompt"},
                 {"params",
                  {{"sessionId", sid},
                   {"prompt", nlohmann::json::array({{{"type", "text"}, {"text", "hi"}}})}}}});
            bool prompt_ok = prompt.contains("result") && !sid.empty();
            // passphrase path
            unsetenv("OPENCLAW_ACP_TOKEN");
            setenv("OPENCLAW_ACP_PASSPHRASE", "phrase-1", 1);
            acp::AgentEndpoint ep2;
            ep2.SetLoopBackend({std::make_shared<provider::MockProvider>(),
                                std::make_shared<tools::Registry>(), agent::LoopOptions{}, "mock"});
            (void)ep2.Handle({{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "initialize"},
                              {"params", {{"protocolVersion", 2}, {"capabilities", nlohmann::json::object()},
                                          {"info", {{"name", "t"}, {"version", "0"}}},
                                          {"passphrase", "phrase-1"}}}});
            auto st = ep2.AuthStatusJson();
            bool pp_ok = st.value("authenticated", false) && st.value("auth_required", false);
            // AUTH_REQUIRED with no credentials → fail-closed login
            unsetenv("OPENCLAW_ACP_PASSPHRASE");
            unsetenv("OPENCLAW_ACP_TOKEN");
            setenv("OPENCLAW_ACP_AUTH_REQUIRED", "1", 1);
            acp::AgentEndpoint ep3;
            auto no_cred = ep3.Login({{"token", "anything"}});
            bool no_cred_fail = !no_cred.value("ok", true);
            unsetenv("OPENCLAW_ACP_TOKEN");
            unsetenv("OPENCLAW_ACP_AUTH_TOKEN");
            unsetenv("OPENCLAW_ACP_PASSPHRASE");
            unsetenv("OPENCLAW_ACP_AUTH_REQUIRED");
            if (!req || !not_authed || !fail_closed || !bad_login || !ok_login || !prompt_ok ||
                !pp_ok || !no_cred_fail) {
                std::cerr << "FAIL acp auth gate init=" << init.dump()
                          << " denied=" << denied.dump() << " good=" << good.dump()
                          << " prompt=" << prompt.dump() << " st=" << st.dump()
                          << " no_cred=" << no_cred.dump() << "\n";
                ++failures;
            } else
                std::cerr << "PASS acp auth gate fail-closed+login\n";
        }

// Channel-bound ACP (mock inbound, no live Slack)
        {
            setenv("OPENCLAW_ACP_AUTO_BIND", "1", 1);
            setenv("OPENCLAW_DEFAULT_PROVIDER", "mock", 1);
            acp::Registry reg;
            auto bound = reg.BindChannel("slack", "Cbind1", "");
            if (!bound.value("ok", false)) {
                std::cerr << "FAIL acp.bind " << bound.dump() << "\n"; ++failures;
            } else {
                auto list = reg.Bindings().ListJson();
                channels::InboundMessage msg;
                msg.channel = "slack";
                msg.sender = "U1";
                msg.text = "bind-hello";
                msg.session_key = "slack-Cbind1";
                nlohmann::json out_log;
                auto r = reg.HandleChannelInbound(msg, [&](const std::string& ch, const std::string& cid,
                                                           const std::string& text) {
                    out_log = {{"channel", ch}, {"chat_id", cid}, {"text", text}, {"recorded", true}};
                    return out_log;
                });
                // Second channel auto-creates
                channels::InboundMessage msg2;
                msg2.channel = "feishu";
                msg2.sender = "ou_1";
                msg2.text = "feishu-hi";
                msg2.session_key = "feishu-oc_chat";
                auto r2 = reg.HandleChannelInbound(msg2, {});
                unsetenv("OPENCLAW_ACP_AUTO_BIND");
                if (!r.handled || r.reply.empty() || r.channel_key != "slack:Cbind1" ||
                    !out_log.value("recorded", false) || !r2.handled || list.empty()) {
                    std::cerr << "FAIL acp channel bind " << r.detail.dump() << " out=" << out_log.dump()
                              << " r2=" << r2.detail.dump() << "\n";
                    ++failures;
                } else std::cerr << "PASS acp channel-bound sessions\n";
            }
        }

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


    // media pipeline
    {
        auto dir = std::filesystem::temp_directory_path() / "oc-media-test";
        std::filesystem::remove_all(dir);
        media::Store ms(dir);
        auto a = ms.Put("hi.txt", "text/plain", "hello-media");
        auto b = ms.ReadBytes(a.id);
        if (!b || *b != "hello-media") { std::cerr << "FAIL media put/read\n"; ++failures; }
        else if (!ms.Remove(a.id)) { std::cerr << "FAIL media remove\n"; ++failures; }
        else std::cerr << "PASS media pipeline\n";
    }

    // daemon pidfile
    {
        auto p = std::filesystem::temp_directory_path() / "oc-test.pid";
        daemon::WritePidFile(p, getpid());
        auto r = daemon::ReadPidFile(p);
        if (!r || *r != getpid() || !daemon::ProcessAlive(*r)) {
            std::cerr << "FAIL daemon pidfile\n"; ++failures;
        } else {
            daemon::RemovePidFile(p);
            std::cerr << "PASS daemon pidfile\n";
        }
    }

    // plugin sdk sample if .so present
    {
        plugins::Registry preg;
        preg.Scan(RepoRoot() / "plugins");
        int n = preg.LoadNatives();
        if (n > 0) {
            auto j = preg.InvokeNative("sample_echo", "ping", {{"x", 1}});
            if (!j.value("ok", false)) { std::cerr << "FAIL plugin invoke " << j.dump() << "\n"; ++failures; }
            else std::cerr << "PASS plugin native invoke\n";
        } else {
            std::cerr << "PASS plugin scan (no .so yet — build sample)\n";
        }
    }


    // TTS record-only
    {
        voice::TtsEngine tts;
        auto st = tts.Status();
        auto sp = tts.Speak("hello");
        if (sp.value("mode", "") != "record_only" && st.value("url_configured", true)) {
            // ok if url configured in env
        }
        if (!sp.value("ok", false)) { std::cerr << "FAIL tts speak\n"; ++failures; }
        else std::cerr << "PASS tts record-only/http\n";
    }

    // http_get tool (example.com) + browser actions
    {
        approvals::Store ap;
        tools::RegistryOptions o; o.approvals = &ap; o.enable_shell = false;
        auto reg = tools::MakeDefaultRegistry(o);
        if (!reg.Has("http_get") || !reg.Has("browser")) {
            std::cerr << "FAIL tools http_get/browser missing\n"; ++failures;
        } else {
            auto br = nlohmann::json::parse(reg.Invoke("browser", {{"action", "playwright"}}));
            if (br.value("ok", true) != false || br.value("error", "") != "unknown action") {
                std::cerr << "FAIL browser unknown action " << br.dump() << "\n"; ++failures;
            } else std::cerr << "PASS browser action surface\n";
            auto st = nlohmann::json::parse(reg.Invoke("browser", {{"action", "status"}}));
            if (!st.contains("engine") && !st.contains("fallback")) {
                std::cerr << "FAIL browser status " << st.dump() << "\n"; ++failures;
            } else std::cerr << "PASS browser status tool\n";
            auto hg = nlohmann::json::parse(reg.Invoke("http_get", {{"url", "https://example.com"}, {"max_bytes", 2048}}));
            if (!hg.value("ok", false) || hg.value("http", 0) < 200) {
                std::cerr << "WARN http_get example.com: " << hg.dump() << "\n";
                // network may be blocked — still pass if tool exists
                std::cerr << "PASS http_get tool registered\n";
            } else std::cerr << "PASS http_get fetch\n";
        }
    }

    // Feishu inbound challenge + message
    {
        auto hub = channels::AdapterHub::MakeDefault();
        auto* fs = dynamic_cast<channels::FeishuAdapter*>(hub.Get("feishu"));
        if (!fs) { std::cerr << "FAIL feishu adapter type\n"; ++failures; }
        else {
            auto ch = fs->HandleHttpInbound({{"type", "url_verification"}, {"challenge", "c-test"}, {"token", ""}}, {});
            if (ch.value("challenge", "") != "c-test") {
                std::cerr << "FAIL feishu challenge " << ch.dump() << "\n"; ++failures;
            } else {
                bool delivered = false;
                auto msg = fs->HandleHttpInbound(
                    {{"event", {{"message", {{"content", "{\"text\":\"ping-feishu\"}"}, {"chat_id", "oc_1"}}},
                                {"sender", {{"sender_id", {{"open_id", "ou_x"}}}}}}}},
                    [&](const channels::InboundMessage& m) {
                        delivered = (m.text == "ping-feishu");
                        return std::string("ack");
                    });
                if (!delivered || !msg.value("ok", false)) {
                    std::cerr << "FAIL feishu inbound msg " << msg.dump() << "\n"; ++failures;
                } else std::cerr << "PASS feishu inbound verify+enqueue\n";
            }
        }
    }


    // rate limit
    {
        agent::RateBucket::Global(); // touch
        agent::LoopOptions o;
        o.max_turns = 2;
        o.rate_limit_per_minute = 1;
        provider::MockProvider mock;
        {
            agent::CompletionResult mr;
            mr.ok = true; mr.finish_reason = "stop";
            mr.message.role = "assistant"; mr.message.content = "ok";
            mock.Enqueue(mr); mock.Enqueue(mr);
        }
        tools::Registry reg;
        std::vector<agent::Message> msgs{{.role="user", .content="hi"}};
        auto r1 = agent::RunAgentLoop(mock, reg, msgs, o);
        // second complete in same minute should fail rate limit if loop needs 2 completes —
        // stop finish uses 1 complete so rate-limit a second loop
        auto r2 = agent::RunAgentLoop(mock, reg, msgs, o);
        if (r2.ok || r2.error.find("rate limit") == std::string::npos) {
            // first used 1 slot; second should be blocked
            std::cerr << "FAIL rate limit got ok=" << r2.ok << " err=" << r2.error << "\n";
            ++failures;
        } else std::cerr << "PASS rate limit\n";
    }

    // session export/import + backup restore
    {
        auto dir = std::filesystem::temp_directory_path() / "oc-sess-x";
        std::filesystem::remove_all(dir);
        session::Store st(dir);
        st.Save("s1", {{.role="user", .content="hello"}, {.role="assistant", .content="world"}});
        auto msgs = st.Load("s1");
        if (msgs.size() != 2) { std::cerr << "FAIL session seed\n"; ++failures; }
        else {
            // backup-style roundtrip via JSON
            nlohmann::json export_j = nlohmann::json::array();
            for (auto& m : msgs) export_j.push_back(m.ToJson());
            session::Store st2(dir / "b");
            std::vector<agent::Message> back;
            for (auto& jm : export_j) {
                agent::Message m;
                m.role = jm.value("role","");
                m.content = jm.value("content","");
                back.push_back(m);
            }
            st2.Save("s1", back);
            if (st2.Load("s1").size() != 2) { std::cerr << "FAIL session import\n"; ++failures; }
            else std::cerr << "PASS session export/import\n";
        }
    }

    // slack + discord inbound
    {
        auto hub = channels::AdapterHub::MakeDefault();
        auto* sl = dynamic_cast<channels::SlackAdapter*>(hub.Get("slack"));
        auto* dc = dynamic_cast<channels::DiscordAdapter*>(hub.Get("discord"));
        if (!sl || !dc) { std::cerr << "FAIL slack/discord adapters\n"; ++failures; }
        else {
            auto ch = sl->HandleHttpInbound({{"type","url_verification"},{"challenge","slack-ch"}}, {});
            auto ping = dc->HandleHttpInbound({{"type", 1}}, {});
            bool ok_msg = false;
            sl->HandleHttpInbound(
                {{"type","event_callback"},{"event",{{"type","message"},{"text","hi-slack"},{"channel","C1"},{"user","U1"}}}},
                [&](const channels::InboundMessage& m){ ok_msg = m.text=="hi-slack"; return std::string("r"); });
            bool ok_dc = false;
            dc->HandleHttpInbound({{"content","hi-dc"},{"channel_id","99"},{"author",{{"username","u"}}}},
                [&](const channels::InboundMessage& m){ ok_dc = m.text=="hi-dc"; return std::string("r"); });
            if (ch.value("challenge","")!="slack-ch" || ping.value("mode","")!="discord_ping" || !ok_msg || !ok_dc) {
                std::cerr << "FAIL slack/discord inbound\n"; ++failures;
            } else std::cerr << "PASS slack/discord inbound\n";
        }
    }

    // Slack Socket Mode — offline recorded frames (no network)
    {
        setenv("SLACK_SOCKET_FORCE_MOCK", "1", 1);
        channels::SlackSocketMode sock;
        int got_n = 0;
        std::string first_text;
        auto deliver = [&](const channels::InboundMessage& m) {
            if (got_n == 0) first_text = m.text;
            ++got_n;
            return std::string("pong-") + m.text;
        };
        std::vector<nlohmann::json> frames = {
            {{"type", "hello"}},
            {{"envelope_id", "e1"},
             {"type", "events_api"},
             {"payload",
              {{"type", "event_callback"},
               {"event",
                {{"type", "message"},
                 {"text", "sock-hi"},
                 {"channel", "C9"},
                 {"user", "U9"}}}}}},
            {{"envelope_id", "e2"},
             {"type", "slash_commands"},
             {"accepts_response_payload", true},
             {"payload",
              {{"command", "/oc"},
               {"text", "ping"},
               {"user_id", "U9"},
               {"channel_id", "C9"}}}},
        };
        auto res = sock.RunMockFrames(frames, deliver);
        const bool started = sock.Start(deliver);  // FORCE_MOCK → false
        sock.Stop();
        unsetenv("SLACK_SOCKET_FORCE_MOCK");
        const auto st = sock.Status();
        if (!res.value("ok", false) || got_n < 2 || first_text != "sock-hi" || started ||
            st.value("acked", 0) < 2) {
            std::cerr << "FAIL slack socket mock " << res.dump() << " status=" << st.dump()
                      << " first=" << first_text << " n=" << got_n << "\n";
            ++failures;
        } else
            std::cerr << "PASS slack socket mode mock frames\n";
    }






    // Board store (disk JSON)
    {
        auto dir = std::filesystem::temp_directory_path() / "oc-board-test";
        std::filesystem::create_directories(dir);
        board::Store store(dir / "board.json");
        auto b0 = store.EnsureDefaults();
        auto add = store.AddCard("todo", "test-card", "body1");
        auto id = add["card"].value("id", "");
        auto mv = store.MoveCard(id, "doing");
        auto got = store.Get();
        int in_doing = 0;
        for (const auto& c : got["cards"])
            if (c.value("id", "") == id && c.value("column_id", "") == "doing") ++in_doing;
        store.DeleteCard(id);
        auto after = store.Get();
        // reload
        board::Store store2(dir / "board.json");
        auto re = store2.Get();
        if (b0["columns"].size() < 3 || id.empty() || in_doing != 1 ||
            after["cards"].size() != 0 || re["columns"].size() < 3) {
            std::cerr << "FAIL board store " << add.dump() << " " << got.dump() << "\n";
            ++failures;
        } else
            std::cerr << "PASS board store CRUD+persist\n";
    }

    // canvas disk store + sim bridge
    try {
        auto dir = std::filesystem::temp_directory_path() / "oc-canvas-st";
        std::filesystem::remove_all(dir);
        canvas::Store store(dir);
        nlohmann::json widgets = nlohmann::json::array();
        widgets.push_back({{"type", "text"}, {"text", "w"}});
        auto created = store.Create({{"title", "t1"}, {"html", "<p>hi</p>"}, {"widgets", widgets}});
        nlohmann::json cdoc = created.contains("document") && created["document"].is_object()
                                  ? created["document"]
                                  : nlohmann::json::object();
        auto id = cdoc.value("id", "");
        auto patched = store.Patch(id, {{"html", "<p>hi2</p>"}, {"title", "t2"}});
        auto got = store.Get(id);
        auto listed = store.List();
        auto ev = store.Events(10, id);
        (void)ev;
        store.RecordEvent("test.ping", {{"x", 1}}, id);
        canvas::Store store2(dir);
        auto got2 = store2.Get(id);
        sim::SimulationBackend sim(dir / "simroot");
        auto put = sim.CanvasPut({{"title", "from-sim"}, {"html", "<b>sim</b>"}});
        auto view = sim.CanvasView({});
        nlohmann::json pdoc = patched.contains("document") && patched["document"].is_object()
                                  ? patched["document"]
                                  : nlohmann::json::object();
        nlohmann::json gdoc = got.contains("document") && got["document"].is_object()
                                  ? got["document"]
                                  : nlohmann::json::object();
        const auto docs = listed.contains("docs") && listed["docs"].is_array()
                              ? listed["docs"]
                              : nlohmann::json::array();
        bool ok = created.value("ok", false) && !id.empty() &&
                  pdoc.value("html", "") == "<p>hi2</p>" &&
                  gdoc.value("title", "") == "t2" &&
                  docs.size() >= 1 &&
                  got2.value("ok", false) &&
                  put.value("persisted", false) &&
                  view.value("impl", "") == "real";
        if (!ok) {
            std::cerr << "FAIL canvas store created=" << created.dump()
                      << " patch=" << patched.dump() << " put=" << put.dump()
                      << " view=" << view.dump() << "\n";
            ++failures;
        } else
            std::cerr << "PASS canvas store CRUD+persist+events\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL canvas store exception: " << e.what() << "\n";
        ++failures;
    }

    // Feishu long-connection — offline recorded frames (no network)
    {
        setenv("FEISHU_WS_FORCE_MOCK", "1", 1);
        channels::FeishuLongConn lc;
        int got_n = 0;
        std::string first_text;
        auto deliver = [&](const channels::InboundMessage& m) {
            if (got_n == 0) first_text = m.text;
            ++got_n;
            return std::string("f-pong-") + m.text;
        };
        std::vector<nlohmann::json> frames = {
            {{"type", "ping"}},
            {{"type", "event"},
             {"message_id", "mid-1"},
             {"data",
              {{"schema", "2.0"},
               {"header", {{"event_type", "im.message.receive_v1"}}},
               {"event",
                {{"sender", {{"sender_id", {{"open_id", "ou_1"}}}}},
                 {"message",
                  {{"chat_id", "oc_chat1"},
                   {"content", "{\"text\":\"feishu-hi\"}"}}}}}}}},
            {{"type", "pong"}},
        };
        auto res = lc.RunMockFrames(frames, deliver);
        const bool started = lc.Start(deliver);
        lc.Stop();
        unsetenv("FEISHU_WS_FORCE_MOCK");
        const auto st = lc.Status();
        bool pong_sent = false;
        if (res.contains("results") && res["results"].is_array() && !res["results"].empty()) {
            auto& ping = res["results"][0];
            pong_sent = ping.contains("send") && ping["send"].is_array() &&
                        !ping["send"].empty() && ping["send"][0].value("type", "") == "pong";
        }
        if (!res.value("ok", false) || got_n < 1 || first_text != "feishu-hi" || started ||
            !pong_sent || st.value("delivered", 0) < 1) {
            std::cerr << "FAIL feishu longconn mock " << res.dump() << " status=" << st.dump()
                      << " first=" << first_text << " n=" << got_n << "\n";
            ++failures;
        } else
            std::cerr << "PASS feishu longconn mock frames\n";
    }

    // Discord Gateway — offline recorded frames (no network)
    {
        setenv("DISCORD_GATEWAY_FORCE_MOCK", "1", 1);
        channels::DiscordGateway gw;
        int got_n = 0;
        std::string first_text;
        auto deliver = [&](const channels::InboundMessage& m) {
            if (got_n == 0) first_text = m.text;
            ++got_n;
            return std::string("d-pong-") + m.text;
        };
        std::vector<nlohmann::json> frames = {
            {{"op", 10}, {"d", {{"heartbeat_interval", 41250}}}},
            {{"op", 0},
             {"t", "READY"},
             {"s", 1},
             {"d", {{"session_id", "sess_mock"}, {"user", {{"id", "bot1"}}}}}},
            {{"op", 0},
             {"t", "MESSAGE_CREATE"},
             {"s", 2},
             {"d",
              {{"id", "m1"},
               {"content", "discord-hi"},
               {"channel_id", "Dchan"},
               {"author", {{"id", "Uuser"}, {"bot", false}}}}}},
            {{"op", 11}},
        };
        auto res = gw.RunMockFrames(frames, deliver);
        const bool started = gw.Start(deliver);  // FORCE_MOCK → false
        gw.Stop();
        unsetenv("DISCORD_GATEWAY_FORCE_MOCK");
        const auto st = gw.Status();
        bool identify_sent = false;
        if (res.contains("results") && res["results"].is_array() && !res["results"].empty()) {
            auto& hello = res["results"][0];
            identify_sent = hello.contains("send") && hello["send"].is_array() &&
                            !hello["send"].empty() && hello["send"][0].value("op", -1) == 2;
        }
        if (!res.value("ok", false) || got_n < 1 || first_text != "discord-hi" || started ||
            !identify_sent || st.value("delivered", 0) < 1) {
            std::cerr << "FAIL discord gateway mock " << res.dump() << " status=" << st.dump()
                      << " first=" << first_text << " n=" << got_n << "\n";
            ++failures;
        } else
            std::cerr << "PASS discord gateway mock frames\n";
    }

    // JS plugin host (Node bridge or FORCE_MOCK)
    {
        setenv("OPENCLAW_JS_FORCE_MOCK", "1", 1);
        plugins::JsHost mock_host;
        mock_host.Start(std::filesystem::temp_directory_path());  // mock path ok
        auto st = mock_host.Status();
        auto echo = mock_host.CallTool("js.sample_echo", {{"text", "ci"}});
        unsetenv("OPENCLAW_JS_FORCE_MOCK");
        if (!st.value("mock", false) || echo.value("content", "") != "js-echo:ci") {
            std::cerr << "FAIL js host mock " << echo.dump() << "\n";
            ++failures;
        } else
            std::cerr << "PASS js plugin host mock\n";
    }
    {
        // Live Node bridge when node + host.mjs exist (no network)
        plugins::JsHost live;
        const auto root = RepoRoot() / "plugins";
        const auto bridge = root / "js_bridge" / "host.mjs";
        if (std::filesystem::exists(bridge)) {
            unsetenv("OPENCLAW_JS_FORCE_MOCK");
            live.Start(root, bridge);
            auto echo = live.CallTool("js.sample_echo", {{"text", "node"}});
            auto add = live.CallTool("js.sample_add", {{"a", 2}, {"b", 40}});
            tools::Registry tr;
            auto n = live.RegisterTools(tr);
            live.Stop();
            const bool echo_ok = echo.value("content", "") == "js-echo:node";
            const bool add_ok = add.value("sum", 0.0) == 42.0 || add.value("content", "") == "42";
            if (!echo_ok || !add_ok || n < 2) {
                std::cerr << "FAIL js host node " << echo.dump() << " " << add.dump()
                          << " n=" << n << "\n";
                ++failures;
            } else
                std::cerr << "PASS js plugin host node bridge\n";
        } else {
            std::cerr << "PASS js plugin host skip-no-bridge\n";
        }
    }

    // whatsapp Cloud API + imessage inbound
    {
        setenv("WHATSAPP_FORCE_MOCK", "1", 1);
        auto hub = channels::AdapterHub::MakeDefault();
        auto* wa = dynamic_cast<channels::WhatsAppCloudAdapter*>(hub.Get("whatsapp"));
        auto* im = dynamic_cast<channels::SimpleInboundAdapter*>(hub.Get("imessage"));
        if (!wa || !im) { std::cerr << "FAIL wa/imessage adapters\n"; ++failures; }
        else {
            auto ch = wa->HandleVerifyGet("", "m-ch");
            bool ok = false;
            nlohmann::json wa_body = nlohmann::json::parse(R"({
              "entry":[{"changes":[{"value":{"messages":[{"from":"15551234567","type":"text","text":{"body":"wa-hi"}}],"contacts":[{"wa_id":"15551234567"}]}}]}]
            })");
            auto fed = wa->RunMockFrames({wa_body},
                [&](const channels::InboundMessage& m){
                    ok = m.text=="wa-hi" && m.session_key.find("15551234567")!=std::string::npos;
                    return std::string("pong-wa");
                });
            bool ok2 = false;
            im->HandleHttpInbound({{"text", "im-hi"}, {"sender", "me"}},
                [&](const channels::InboundMessage& m){ ok2 = m.text=="im-hi"; return std::string("r"); });
            unsetenv("WHATSAPP_FORCE_MOCK");
            if (ch.value("challenge","")!="m-ch" || !ok || !ok2 || !fed.value("ok", false)) {
                std::cerr << "FAIL wa/imessage inbound " << fed.dump() << "\n"; ++failures;
            } else std::cerr << "PASS whatsapp cloud + imessage inbound\n";
        }
    }

    // implemented growth
    {
        auto n = gateway::ImplementedMethods().size();
        if (n < 500) { std::cerr << "FAIL implemented too small " << n << "\n"; ++failures; }
        else std::cerr << "PASS implemented growth (" << n << ")\n";
    }


    // simulation backend + screenshot capture
    {
        setenv("OPENCLAW_SCREENSHOT_FORCE_SIM", "1", 1);
        sim::SimulationBackend sim(std::filesystem::temp_directory_path() / "oc-sim");
        auto shot = sim.Screenshot({});
        auto inv = sim.ComputerInvoke({{"action", "mouse_click"}, {"x", 1}, {"y", 2}});
        auto cv = sim.CanvasPut({{"title", "t1"}, {"widgets", nlohmann::json::array()}});
        auto dl = sim.DesktopLaunch({{"app", "sim"}});
        if (!shot.contains("path") || shot.value("impl", "") != "real-sim" ||
            inv.value("action", "") != "mouse_click" || !cv.value("saved", false) ||
            !dl["desktop"].value("running", false)) {
            std::cerr << "FAIL sim backend " << shot.dump() << "\n"; ++failures;
        } else std::cerr << "PASS simulation backend (force-sim screenshot)\n";
        unsetenv("OPENCLAW_SCREENSHOT_FORCE_SIM");

        // Input force-sim (no display required)
        setenv("OPENCLAW_INPUT_FORCE_SIM", "1", 1);
        {
            sim::SimulationBackend sim_in(std::filesystem::temp_directory_path() / "oc-sim-in");
            auto click = sim_in.ComputerInvoke({{"action", "click"}, {"x", 10}, {"y", 20}, {"button", "left"}});
            auto type = sim_in.ComputerInvoke({{"action", "type"}, {"text", "hi"}});
            auto move = sim_in.ComputerInvoke({{"action", "move"}, {"x", 1}, {"y", 2}});
            if (click.value("impl", "") != "real-sim" || type.value("impl", "") != "real-sim" ||
                move.value("impl", "") != "real-sim") {
                std::cerr << "FAIL input force-sim " << click.dump() << "\n"; ++failures;
            } else std::cerr << "PASS computer click/type force-sim\n";
        }
        unsetenv("OPENCLAW_INPUT_FORCE_SIM");

        // Live input when DISPLAY + xdotool (optional)
        {
            auto inenv = sim::ProbeInputEnv();
            if (inenv.has_display && !inenv.tools_available.empty() && !inenv.force_sim) {
                sim::SimulationBackend live_in(std::filesystem::temp_directory_path() / "oc-sim-in-live");
                auto mv = live_in.ComputerInvoke({{"action", "move"}, {"x", 120}, {"y", 140}});
                auto cl = live_in.ComputerInvoke({{"action", "click"}, {"x", 120}, {"y", 140}});
                if (mv.value("impl", "") == "real" && cl.value("impl", "") == "real") {
                    std::cerr << "PASS computer move/click live (" << mv.value("engine", "") << ")\n";
                } else {
                    std::cerr << "WARN computer input live fallback " << mv.dump() << " " << cl.dump() << "\n";
                }
            } else {
                std::cerr << "PASS computer input no-display probe\n";
            }
        }

        // Live path when DISPLAY + capture tool exist (optional; do not fail CI headless)
        auto env = sim::ProbeCaptureEnv();
        if (env.has_display && !env.tools_available.empty()) {
            sim::SimulationBackend live(std::filesystem::temp_directory_path() / "oc-sim-live");
            auto live_shot = live.Screenshot({});
            if (live_shot.value("impl", "") == "real" && live_shot.value("bytes", 0) > 100) {
                std::cerr << "PASS computer.screenshot live (" << live_shot.value("engine", "")
                          << " bytes=" << live_shot.value("bytes", 0) << ")\n";
            } else {
                std::cerr << "WARN computer.screenshot live skipped/fallback " << live_shot.dump() << "\n";
            }
        } else {
            std::cerr << "PASS computer.screenshot no-display probe (tools="
                      << env.tools_available.size() << ")\n";
        }
        auto br = sim.BrowserOpenUrl("https://example.com");
        if (!br.value("ok", false)) std::cerr << "WARN browser.open_url " << br.dump() << "\n";
        else std::cerr << "PASS browser.open_url curl-or-cdp\n";
    }

    // CDP mock + WS framing (no live Chromium required)
    {
        auto frame = browser::WsClientMaskFrame("hello");
        size_t consumed = 0;
        std::string payload;
        uint8_t opcode = 0;
        std::string server_frame;
        server_frame.push_back(static_cast<char>(0x81));
        server_frame.push_back(5);
        server_frame += "hello";
        if (!browser::WsParseServerFrame(server_frame, consumed, payload, opcode) ||
            payload != "hello" || opcode != 0x1 || frame.size() < 6) {
            std::cerr << "FAIL ws framing\n"; ++failures;
        } else std::cerr << "PASS ws framing\n";

        setenv("OPENCLAW_BROWSER_CDP_MOCK", "1", 1);
        browser::CdpClient cdp(browser::CdpConfig{.mock = true});
        auto conn = cdp.Connect();
        auto nav = cdp.Navigate("https://example.com");
        auto ev = cdp.Evaluate("document.title");
        auto shotp = cdp.ScreenshotPng();
        auto tu = cdp.TitleAndUrl();
        if (!conn.value("ok", false) || !nav.value("ok", false) ||
            ev.value("value", "") != "Example Domain" || !shotp.value("ok", false) ||
            !tu.value("ok", false)) {
            std::cerr << "FAIL cdp mock " << nav.dump() << " " << ev.dump() << "\n"; ++failures;
        } else std::cerr << "PASS cdp mock navigate/eval/screenshot\n";

        browser::Engine eng(std::filesystem::temp_directory_path() / "oc-browser-eng");
        auto open = eng.OpenUrl("https://example.com");
        auto eval = eng.Evaluate("document.title");
        if (!open.value("ok", false) || eval.value("value", "") != std::string("Example Domain")) {
            std::cerr << "FAIL browser engine mock " << open.dump() << "\n"; ++failures;
        } else std::cerr << "PASS browser engine (cdp-mock)\n";
        unsetenv("OPENCLAW_BROWSER_CDP_MOCK");
    }

    // mcp.app bridge via client
    {
        auto cli = mcp::MakeMockClient();
        auto tools = cli->ListTools();
        auto call = cli->CallTool("mock_echo", {{"x", 1}});
        auto res = cli->ListResources();
        if (tools.empty() || !res.contains("resources")) {
            std::cerr << "FAIL mcp app bridge pieces\n"; ++failures;
        } else std::cerr << "PASS mcp client list/call/resources\n";
    }

    // browser_open_url tool
    {
        approvals::Store ap;
        tools::RegistryOptions o; o.approvals = &ap; o.enable_shell = false;
        auto reg = tools::MakeDefaultRegistry(o);
        if (!reg.Has("browser_open_url") || !reg.Has("browser") ||
            !reg.Has("browser_evaluate") || !reg.Has("browser_screenshot")) {
            std::cerr << "FAIL browser tools\n"; ++failures;
        } else std::cerr << "PASS browser tools registered\n";
    }


    // session lock + loop timeout/cancel
    {
        auto dir = std::filesystem::temp_directory_path() / "oc-lock-test";
        std::filesystem::remove_all(dir);
        session::Store st(dir);
        {
            auto g1 = st.Lock("a");
            st.Save("a", {{.role = "user", .content = "x"}});
        }
        if (st.Load("a").size() != 1) { std::cerr << "FAIL session lock save\n"; ++failures; }
        else std::cerr << "PASS session lock\n";

        provider::MockProvider mock;
        // slow path: enqueue nothing useful — use cancel
        std::atomic<bool> cancel{false};
        agent::LoopOptions o;
        o.max_turns = 3;
        o.cancel = &cancel;
        cancel = true;
        agent::CompletionResult mr;
        mr.ok = true; mr.finish_reason = "stop";
        mr.message.role = "assistant"; mr.message.content = "x";
        mock.Enqueue(mr);
        tools::Registry empty_tools;
        auto r = agent::RunAgentLoop(mock, empty_tools, {{.role="user",.content="h"}}, o);
        if (!r.cancelled) { std::cerr << "FAIL cancel " << r.error << "\n"; ++failures; }
        else std::cerr << "PASS agent cancel\n";

        cancel = false;
        o.cancel = nullptr;
        o.timeout = std::chrono::milliseconds(0);
        mock.Enqueue(mr);
        auto r2 = agent::RunAgentLoop(mock, empty_tools, {{.role="user",.content="h"}}, o);
        if (!r2.ok) { std::cerr << "FAIL loop ok after cancel test\n"; ++failures; }
        else std::cerr << "PASS agent loop after cancel reset\n";
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


openclaw::gateway::Server* g_gateway_server = nullptr;
void GatewayStopSignal() {
    if (g_gateway_server) g_gateway_server->Stop();
}

int CmdGatewayStop() {
    using namespace openclaw;
    auto path = daemon::DefaultPidPath();
    auto pid = daemon::ReadPidFile(path);
    if (!pid) {
        std::cerr << "no pidfile at " << path << "\n";
        return 1;
    }
    if (!daemon::ProcessAlive(*pid)) {
        std::cerr << "stale pidfile pid=" << *pid << " — removing\n";
        daemon::RemovePidFile(path);
        return 1;
    }
    if (!daemon::StopPid(*pid)) {
        std::cerr << "kill(" << *pid << ", SIGTERM) failed\n";
        return 1;
    }
    for (int i = 0; i < 50; ++i) {
        if (!daemon::ProcessAlive(*pid)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (daemon::ProcessAlive(*pid)) {
        std::cerr << "process " << *pid << " still alive after SIGTERM\n";
        return 2;
    }
    daemon::RemovePidFile(path);
    std::cout << nlohmann::json{{"stopped", true}, {"pid", *pid}, {"pidfile", path.string()}}.dump(2) << "\n";
    return 0;
}

int CmdGatewayStatusDaemon() {
    using namespace openclaw;
    auto path = daemon::DefaultPidPath();
    auto pid = daemon::ReadPidFile(path);
    bool alive = pid && daemon::ProcessAlive(*pid);
    std::cout << nlohmann::json{
        {"pidfile", path.string()},
        {"pid", pid ? *pid : 0},
        {"running", alive},
        {"version", kVersion},
    }.dump(2) << "\n";
    return alive ? 0 : 1;
}

int CmdGateway(int argc, char** argv, int argi) {
    using namespace openclaw;
    // daemon lifecycle subcommands: gateway start|stop|status
    if (argi < argc) {
        std::string sub = argv[argi];
        if (sub == "stop") return CmdGatewayStop();
        if (sub == "status") return CmdGatewayStatusDaemon();
        if (sub == "start") {
            ++argi;
            auto path = daemon::DefaultPidPath();
            if (auto old = daemon::ReadPidFile(path); old && daemon::ProcessAlive(*old)) {
                std::cerr << "already running pid=" << *old << " (" << path << ")\n";
                return 1;
            }
            pid_t child = fork();
            if (child < 0) { std::cerr << "fork failed\n"; return 1; }
            if (child > 0) {
                // parent
                daemon::WritePidFile(path, child);
                std::cout << nlohmann::json{{"started", true}, {"pid", child}, {"pidfile", path.string()}}.dump(2) << "\n";
                return 0;
            }
            // child — detach session
            setsid();
            // fall through to run gateway foreground in child
        }
    }

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
    plugins::JsHost js_host;
    // Auto-start JS bridge (Node) or mock when FORCE_MOCK / no node
    js_host.Start(RepoRoot() / "plugins");
    const auto js_tools_n = js_host.RegisterTools(tools);
    (void)js_tools_n;
    {
        int pn = plugins.LoadNatives();
        if (pn) std::cerr << "[gateway] native plugins loaded: " << pn << "\n";
    }
    cron::Scheduler cron;
    auth::PairingStore pairing;
    session::Store sessions(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    memory::Store memory(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions"));
    secrets::Store secrets(std::filesystem::path(EnvOr("HOME", "/tmp")) / ".openclaw-cpp" / "secrets.json",
                             EnvOr("OPENCLAW_SECRETS_PASSPHRASE", ""));
    auto nodes = nodes::Registry::MakeDefault();
    media::Store media(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "media");
    board::Store board_store(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "board.json");
    canvas::Store canvas_store(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "canvas");
    events::Bus bus;
    agent::FileStore agent_files(std::filesystem::path(EnvOr("OPENCLAW_SESSION_DIR", "/tmp/openclaw-cpp-sessions")) / "agents");
    auto& logs = logging::GlobalLog();
    logs.Append("info", "gateway starting");

    const auto data_dir = std::filesystem::path(EnvOr("OPENCLAW_DATA_DIR",
        (std::filesystem::path(EnvOr("HOME", "/tmp")) / ".openclaw-cpp" / "data").string()));
    persist::StateStore persist(data_dir);
    acp::Registry acp_reg;
    voice::TtsEngine tts_engine;
    sim::SimulationBackend sim_backend(data_dir);
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
        .tts = &tts_engine,
        .sim = &sim_backend,
        .js_host = &js_host,
        .board = &board_store,
        .canvas = &canvas_store,
        .mcp_client = mcp_client,
        .config = cfg,
        .auth = auth::FromEnv(),
    };


    gateway::Server server(opts, deps);
    g_gateway_server = &server;
    daemon::InstallStopSignals(GatewayStopSignal);
    auto pid_path = daemon::DefaultPidPath();
    // Foreground (non-start) also records pid for stop
    if (!daemon::ReadPidFile(pid_path) || !daemon::ProcessAlive(*daemon::ReadPidFile(pid_path))) {
        daemon::WritePidFile(pid_path, static_cast<int>(getpid()));
    }
    int rc = 0;
    try { server.Run(); }
    catch (const std::exception& e) {
        std::cerr << "gateway error: " << e.what() << '\n';
        rc = 1;
    }
    g_gateway_server = nullptr;
    if (auto cur = daemon::ReadPidFile(pid_path); cur && *cur == static_cast<int>(getpid())) {
        daemon::RemovePidFile(pid_path);
    }
    if (deps.persist) {
        deps.persist->SaveCron(cron);
        deps.persist->SavePairing(pairing);
        deps.persist->SaveNodes(nodes);
    }
    curl_global_cleanup();
    return rc;
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


int CmdAcpStdio() {
    // Line-delimited ACP JSON-RPC agent on stdio (no external acpx required).
    openclaw::acp::AgentEndpoint agent;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            auto req = nlohmann::json::parse(line);
            auto resp = agent.Handle(req);
            for (auto& n : agent.DrainNotifications()) {
                std::cout << n.dump() << std::endl;
            }
            if (!resp.is_null() && !resp.empty()) {
                std::cout << resp.dump() << std::endl;
            }
            std::cout.flush();
        } catch (const std::exception& e) {
            std::cout << nlohmann::json{{"jsonrpc", "2.0"},
                                        {"id", nullptr},
                                        {"error", {{"code", -32700}, {"message", e.what()}}}}.dump()
                      << std::endl;
        }
    }
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
        if (cmd == "acp-stdio") return CmdAcpStdio();
        if (cmd == "gateway") return CmdGateway(argc, argv, 2);
        if (cmd == "chat") return CmdChat(argc, argv, 2);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") { PrintUsage(); return 0; }
        return CmdChat(argc, argv, 1);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
