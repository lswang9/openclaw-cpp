#include "openclaw/acp/wire.hpp"
#include "openclaw/mcp/bridge.hpp"
#include "openclaw/mcp/stdio.hpp"
#include "openclaw/provider/mock.hpp"
#include "openclaw/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <queue>
#include <stdexcept>
#include <thread>

namespace openclaw::acp {
namespace {

nlohmann::json AgentCapabilities() {
    return {{"session",
             {{"prompt", {{"image", true}, {"file", true}}},
              {"_meta",
               {{"openclawCpp",
                 {{"sessionMcp", true},
                  {"auth", true},
                  {"note",
                   "subset — AgentLoop; multimodal; session MCP; token/passphrase auth; no acpx"}}}}}}}};
}

int TurnDelayMs() {
    const char* v = std::getenv("OPENCLAW_ACP_TURN_DELAY_MS");
    if (!v || !*v) return 0;
    return std::atoi(v);
}

bool EnvTruthyLocal(const char* k, bool default_val = false) {
    const char* v = std::getenv(k);
    if (!v || !*v) return default_val;
    const std::string s(v);
    if (s == "0" || s == "false" || s == "no") return false;
    if (s == "1" || s == "true" || s == "yes") return true;
    return default_val;
}

}  // namespace

AgentEndpoint::AgentEndpoint(std::string agent_name, std::string agent_version)
    : name_(std::move(agent_name)), version_(std::move(agent_version)) {}

void AgentEndpoint::SetLoopBackend(LoopBackend backend) {
    std::lock_guard lock(mu_);
    backend_ = std::move(backend);
    backend_ready_ = backend_.provider != nullptr;
}

void AgentEndpoint::EnsureDefaultBackend() {
    if (backend_ready_ && backend_.provider) return;
    auto mock = std::make_shared<provider::MockProvider>();
    auto tools = std::make_shared<tools::Registry>();
    agent::LoopOptions opts;
    opts.max_turns = 4;
    // Prefer OPENCLAW_PROVIDER, then OPENCLAW_DEFAULT_PROVIDER (gateway convention).
    std::string pname = EnvOr("OPENCLAW_PROVIDER", EnvOr("OPENCLAW_DEFAULT_PROVIDER", "mock"));
    backend_ = LoopBackend{mock, tools, opts, pname.empty() ? "mock" : pname};
    backend_ready_ = true;
    // Note: non-mock providers can be injected via SetLoopBackend from CLI/gateway later.
}

bool AgentEndpoint::EnvAuthRequired() {
    return EnvTruthyLocal("OPENCLAW_ACP_AUTH_REQUIRED", false);
}

std::string AgentEndpoint::EnvToken() {
    const char* a = std::getenv("OPENCLAW_ACP_TOKEN");
    if (a && *a) return a;
    const char* b = std::getenv("OPENCLAW_ACP_AUTH_TOKEN");
    if (b && *b) return b;
    return {};
}

std::string AgentEndpoint::EnvPassphrase() {
    const char* p = std::getenv("OPENCLAW_ACP_PASSPHRASE");
    if (p && *p) return p;
    return {};
}

bool AgentEndpoint::CredentialsConfigured() const {
    return !EnvToken().empty() || !EnvPassphrase().empty();
}

bool AgentEndpoint::AuthRequired() const {
    // Required when explicitly forced, or when a token/passphrase is configured.
    return EnvAuthRequired() || CredentialsConfigured();
}

bool AgentEndpoint::Authenticated() const {
    std::lock_guard lock(mu_);
    if (!AuthRequired()) return true;
    return authenticated_;
}

bool AgentEndpoint::CheckCredential(const std::string& token, const std::string& passphrase) const {
    const auto expect_tok = EnvToken();
    const auto expect_pp = EnvPassphrase();
    if (!expect_tok.empty() && !token.empty() && token == expect_tok) return true;
    if (!expect_pp.empty() && !passphrase.empty() && passphrase == expect_pp) return true;
    return false;
}

nlohmann::json AgentEndpoint::AuthMethodsJson() const {
    nlohmann::json arr = nlohmann::json::array();
    if (!EnvToken().empty() || EnvAuthRequired()) {
        arr.push_back({{"id", "token"},
                       {"name", "Bearer / ACP token"},
                       {"description", "OPENCLAW_ACP_TOKEN — pass as authToken / token / Bearer"}});
    }
    if (!EnvPassphrase().empty() || EnvAuthRequired()) {
        arr.push_back({{"id", "passphrase"},
                       {"name", "Passphrase"},
                       {"description", "OPENCLAW_ACP_PASSPHRASE"}});
    }
    return arr;
}

bool AgentEndpoint::AllowMethodWithoutAuth(const std::string& method) const {
    if (method == "initialize" || method == "ping") return true;
    if (method == "auth/login" || method == "auth/authenticate" || method == "authenticate" ||
        method == "auth/status" || method == "auth/logout")
        return true;
    return false;
}

nlohmann::json AgentEndpoint::AuthStatusJson() const {
    std::lock_guard lock(mu_);
    const bool req = AuthRequired();
    return {{"impl", "real"},
            {"auth_required", req},
            {"authenticated", !req || authenticated_},
            {"auth_method", auth_method_},
            {"token_configured", !EnvToken().empty()},
            {"passphrase_configured", !EnvPassphrase().empty()},
            {"force_required_env", EnvAuthRequired()},
            {"authMethods", AuthMethodsJson()},
            {"note", "minimal ACP auth gate — not OAuth/device-code"}};
}

nlohmann::json AgentEndpoint::Login(const nlohmann::json& params) {
    std::lock_guard lock(mu_);
    std::string token;
    std::string passphrase;
    if (params.contains("token") && params["token"].is_string()) token = params["token"].get<std::string>();
    if (params.contains("authToken") && params["authToken"].is_string())
        token = params["authToken"].get<std::string>();
    if (params.contains("bearer") && params["bearer"].is_string()) token = params["bearer"].get<std::string>();
    if (params.contains("passphrase") && params["passphrase"].is_string())
        passphrase = params["passphrase"].get<std::string>();
    if (params.contains("password") && params["password"].is_string())
        passphrase = params["password"].get<std::string>();
    if (params.contains("auth") && params["auth"].is_object()) {
        auto& a = params["auth"];
        if (a.contains("token") && a["token"].is_string()) token = a["token"].get<std::string>();
        if (a.contains("passphrase") && a["passphrase"].is_string())
            passphrase = a["passphrase"].get<std::string>();
    }
    auto method = params.value("method", params.value("type", ""));
    if (method == "token" && token.empty() && params.contains("value") && params["value"].is_string())
        token = params["value"].get<std::string>();
    if (method == "passphrase" && passphrase.empty() && params.contains("value") &&
        params["value"].is_string())
        passphrase = params["value"].get<std::string>();

    if (!AuthRequired()) {
        authenticated_ = true;
        auth_method_ = "none";
        return {{"ok", true}, {"authenticated", true}, {"auth_required", false}, {"impl", "real"}};
    }
    if (!CredentialsConfigured()) {
        authenticated_ = false;
        auth_method_.clear();
        return {{"ok", false},
                {"authenticated", false},
                {"auth_required", true},
                {"error", "auth required but no OPENCLAW_ACP_TOKEN/PASSPHRASE configured (fail-closed)"},
                {"impl", "real"}};
    }
    if (!CheckCredential(token, passphrase)) {
        authenticated_ = false;
        auth_method_.clear();
        return {{"ok", false},
                {"authenticated", false},
                {"auth_required", true},
                {"error", "invalid token or passphrase"},
                {"impl", "real"}};
    }
    authenticated_ = true;
    if (!token.empty() && CheckCredential(token, "")) auth_method_ = "token";
    else auth_method_ = "passphrase";
    return {{"ok", true},
            {"authenticated", true},
            {"auth_required", true},
            {"auth_method", auth_method_},
            {"impl", "real"}};
}

bool AgentEndpoint::TryAuthenticateParams(const nlohmann::json& params) {
    if (!AuthRequired()) return true;
    {
        std::lock_guard lock(mu_);
        if (authenticated_) return true;
    }
    auto r = Login(params);
    return r.value("ok", false) && r.value("authenticated", false);
}

nlohmann::json AgentEndpoint::Ok(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json r{{"jsonrpc", "2.0"}, {"result", std::move(result)}};
    if (!id.is_null()) r["id"] = id;
    return r;
}

nlohmann::json AgentEndpoint::Err(const nlohmann::json& id, int code, const std::string& message,
                                  nlohmann::json data) {
    nlohmann::json err{{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = std::move(data);
    nlohmann::json r{{"jsonrpc", "2.0"}, {"error", std::move(err)}};
    if (!id.is_null()) r["id"] = id;
    return r;
}

void AgentEndpoint::Notify(const std::string& method, nlohmann::json params) {
    pending_notifications_.push_back(
        {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

WireSession* AgentEndpoint::Find(const std::string& id) {
    for (auto& s : sessions_) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string AgentEndpoint::PromptText(const nlohmann::json& prompt_blocks) {
    return ParsePrompt(prompt_blocks).text;
}

AgentEndpoint::PromptParsed AgentEndpoint::ParsePrompt(const nlohmann::json& prompt_blocks) {
    PromptParsed out;
    auto push_text = [&](const std::string& text) {
        if (text.empty()) return;
        if (!out.text.empty()) out.text += "\n";
        out.text += text;
        out.openai_parts.push_back({{"type", "text"}, {"text", text}});
        out.notify_parts.push_back({{"type", "text"}, {"text", text}});
    };
    auto push_image_data_url = [&](const std::string& data_url, const std::string& mime,
                                   const std::string& raw_b64) {
        ++out.images;
        std::string url = data_url;
        if (url.empty() && !raw_b64.empty()) {
            std::string m = mime.empty() ? "image/png" : mime;
            url = "data:" + m + ";base64," + raw_b64;
        }
        if (url.empty()) return;
        out.openai_parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
        nlohmann::json note{{"type", "image"}, {"mimeType", mime.empty() ? "image/png" : mime}};
        if (!raw_b64.empty()) note["data"] = raw_b64.substr(0, std::min<size_t>(raw_b64.size(), 32)) +
                                             (raw_b64.size() > 32 ? "…" : "");
        else note["url"] = url.substr(0, std::min<size_t>(url.size(), 64));
        out.notify_parts.push_back(note);
        if (out.text.find("[image]") == std::string::npos) {
            // keep text field informative for non-vision sinks
            if (!out.text.empty()) out.text += "\n";
            out.text += "[image]";
        }
    };
    auto push_file = [&](const std::string& name, const std::string& mime, const std::string& b64) {
        ++out.files;
        out.notify_parts.push_back({{"type", "file"},
                                    {"name", name},
                                    {"mimeType", mime},
                                    {"bytes_b64", b64.size()}});
        if (!b64.empty() && mime.rfind("image/", 0) == 0) {
            push_image_data_url("", mime, b64);
            return;
        }
        std::string marker = "[file " + (name.empty() ? "attachment" : name) +
                             (mime.empty() ? "" : (" " + mime)) +
                             " b64_len=" + std::to_string(b64.size()) + "]";
        if (!out.text.empty()) out.text += "\n";
        out.text += marker;
        out.openai_parts.push_back({{"type", "text"}, {"text", marker}});
    };

    if (prompt_blocks.is_string()) {
        push_text(prompt_blocks.get<std::string>());
        return out;
    }
    if (!prompt_blocks.is_array()) return out;

    for (const auto& b : prompt_blocks) {
        if (b.is_string()) {
            push_text(b.get<std::string>());
            continue;
        }
        if (!b.is_object()) continue;
        auto type = b.value("type", "");
        if (type == "text" || (type.empty() && b.contains("text") && !b.contains("data") &&
                               !b.contains("image_url") && !b.contains("source"))) {
            push_text(b.value("text", ""));
            continue;
        }
        // ACP / openclaw: image with base64 data
        if (type == "image" || type == "image_base64" || type == "input_image") {
            std::string mime = b.value("mimeType", b.value("media_type", b.value("mime", "image/png")));
            std::string data = b.value("data", b.value("base64", b.value("image_base64", "")));
            std::string url = b.value("url", "");
            if (b.contains("image_url")) {
                if (b["image_url"].is_string()) url = b["image_url"].get<std::string>();
                else if (b["image_url"].is_object()) url = b["image_url"].value("url", url);
            }
            if (b.contains("source") && b["source"].is_object()) {
                auto& src = b["source"];
                if (src.value("type", "") == "base64") {
                    data = src.value("data", data);
                    mime = src.value("media_type", src.value("mimeType", mime));
                } else if (src.value("type", "") == "url") {
                    url = src.value("url", url);
                }
            }
            if (!url.empty() && url.rfind("data:", 0) == 0) push_image_data_url(url, mime, "");
            else if (!data.empty()) push_image_data_url("", mime, data);
            else if (!url.empty()) {
                ++out.images;
                out.openai_parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
                out.notify_parts.push_back({{"type", "image"}, {"url", url}});
                if (!out.text.empty()) out.text += "\n";
                out.text += "[image]";
            }
            continue;
        }
        if (type == "image_url") {
            std::string url;
            if (b.contains("image_url")) {
                if (b["image_url"].is_string()) url = b["image_url"].get<std::string>();
                else url = b["image_url"].value("url", "");
            } else url = b.value("url", "");
            if (!url.empty()) {
                if (url.rfind("data:", 0) == 0) push_image_data_url(url, "image/png", "");
                else {
                    ++out.images;
                    out.openai_parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
                    out.notify_parts.push_back({{"type", "image"}, {"url", url}});
                    if (!out.text.empty()) out.text += "\n";
                    out.text += "[image]";
                }
            }
            continue;
        }
        if (type == "file" || type == "input_file" || type == "blob") {
            push_file(b.value("name", b.value("filename", "")),
                      b.value("mimeType", b.value("media_type", "")),
                      b.value("data", b.value("base64", "")));
            continue;
        }
        if (type == "resource_link" || type == "resource") {
            push_text("[resource " +
                      b.value("uri", b.value("resource", nlohmann::json::object()).value("uri", "")) +
                      "]");
            continue;
        }
        // Fallback: unknown object with text
        if (b.contains("text")) push_text(b.value("text", ""));
    }
    // If only images and text is just [image] markers, fine.
    // Ensure openai_parts non-empty when we have text-only — already handled.
    // If we have multimodal parts but OpenAI parts empty somehow, synthesize from text.
    if (out.openai_parts.empty() && !out.text.empty()) {
        out.openai_parts.push_back({{"type", "text"}, {"text", out.text}});
    }
    return out;
}

std::vector<nlohmann::json> AgentEndpoint::DrainNotifications() {
    std::lock_guard lock(mu_);
    std::vector<nlohmann::json> out;
    out.swap(pending_notifications_);
    return out;
}

nlohmann::json AgentEndpoint::StatusJson() const {
    std::lock_guard lock(mu_);
    return {{"impl", "real"},
            {"wire", true},
            {"agent_loop", true},
            {"protocolVersion", kAcpProtocolVersion},
            {"initialized", initialized_},
            {"agent", {{"name", name_}, {"version", version_}}},
            {"provider", backend_ready_ ? backend_.provider_name : "mock(default)"},
            {"sessions", sessions_.size()},
            {"capabilities", AgentCapabilities()},
            {"gap_doc", "docs/ACP-GAP.md"},
            {"session_mcp", true},{"auth_required", AuthRequired()},{"authenticated", !AuthRequired() || authenticated_},{"note", "session/prompt → RunAgentLoop; multimodal; session MCP; auth gate; channel bind; not acpx"}};
}

nlohmann::json AgentEndpoint::ListSessionsJson() const {
    std::lock_guard lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions_) {
        arr.push_back({{"sessionId", s.id},
                       {"cwd", s.cwd},
                       {"status", s.status},
                       {"messages", s.messages.size()},
                       {"history", s.history.size()},
                       {"mcp_servers", s.mcp_servers.size()},
                       {"cancel_requested", s.cancel && s.cancel->load()}});
    }
    return arr;
}

const WireSession* AgentEndpoint::FindConst(const std::string& id) const {
    for (const auto& s : sessions_) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

void AgentEndpoint::AttachMcpServersFromParams(WireSession& session, const nlohmann::json& mcp_servers) {
    // Caller must hold mu_. Specs use mock/stdio; failures are skipped (logged via empty tool list).
    if (!mcp_servers.is_array()) return;
    for (const auto& spec : mcp_servers) {
        if (!spec.is_object()) continue;
        auto one = AddSessionMcpTo(session, spec);
        (void)one;
    }
}

nlohmann::json AgentEndpoint::AddSessionMcpTo(WireSession& s, const nlohmann::json& spec) {
    SessionMcpAttachment att;
    att.id = "mcp_" + std::to_string(mcp_seq_++);
    att.name = spec.value("name", spec.value("id", "mcp"));
    att.transport = spec.value("transport", "");
    if (att.transport.empty()) {
        if (spec.contains("command") || spec.contains("argv") || spec.contains("args"))
            att.transport = "stdio";
        else
            att.transport = "mock";
    }
    att.prefix = spec.value("prefix", "mcp.");
    if (!att.prefix.empty() && att.prefix.back() != '.') att.prefix.push_back('.');

    if (att.transport == "mock" || att.transport == "in-process" || att.transport == "force_mock") {
        att.client = mcp::MakeMockClient();
    } else if (att.transport == "stdio") {
        std::vector<std::string> argv;
        if (spec.contains("argv") && spec["argv"].is_array()) {
            for (const auto& a : spec["argv"]) {
                if (a.is_string()) argv.push_back(a.get<std::string>());
            }
        } else if (spec.contains("args") && spec["args"].is_array()) {
            if (spec.contains("command") && spec["command"].is_string())
                argv.push_back(spec["command"].get<std::string>());
            for (const auto& a : spec["args"]) {
                if (a.is_string()) argv.push_back(a.get<std::string>());
            }
        } else if (spec.contains("command") && spec["command"].is_string()) {
            argv.push_back(spec["command"].get<std::string>());
        }
        att.argv = argv;
        if (argv.empty()) {
            return {{"ok", false}, {"error", "stdio MCP requires command/argv"}, {"sessionId", s.id}};
        }
        try {
            att.client = mcp::MakeStdioClient(argv);
        } catch (const std::exception& e) {
            return {{"ok", false}, {"error", e.what()}, {"transport", "stdio"}};
        }
    } else {
        return {{"ok", false},
                {"error", "unsupported transport (use mock|stdio)"},
                {"transport", att.transport}};
    }

    if (!att.client) return {{"ok", false}, {"error", "failed to create MCP client"}};

    tools::Registry probe;
    auto n = mcp::RegisterMcpTools(probe, att.client, att.prefix);
    att.tool_names = probe.Names();

    s.mcp_servers.push_back(std::move(att));
    const auto& back = s.mcp_servers.back();
    return {{"ok", true},
            {"impl", "real"},
            {"sessionId", s.id},
            {"mcp",
             {{"id", back.id},
              {"name", back.name},
              {"transport", back.transport},
              {"prefix", back.prefix},
              {"tools", back.tool_names},
              {"tool_count", n}}},
            {"mcp_servers", s.mcp_servers.size()}};
}

std::shared_ptr<tools::Registry> AgentEndpoint::BuildTurnTools(const WireSession& session) const {
    auto merged = std::make_shared<tools::Registry>();
    if (backend_.tools) merged->MergeFrom(*backend_.tools);
    for (const auto& att : session.mcp_servers) {
        if (!att.client) continue;
        mcp::RegisterMcpTools(*merged, att.client, att.prefix);
    }
    return merged;
}

nlohmann::json AgentEndpoint::AddSessionMcp(const std::string& session_id, const nlohmann::json& spec) {
    std::lock_guard lock(mu_);
    auto* s = Find(session_id);
    if (!s) return {{"ok", false}, {"error", "session not found"}, {"sessionId", session_id}};
    return AddSessionMcpTo(*s, spec);
}

nlohmann::json AgentEndpoint::ListSessionMcp(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    auto* s = FindConst(session_id);
    if (!s) return {{"ok", false}, {"error", "session not found"}, {"sessionId", session_id}};
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : s->mcp_servers) {
        arr.push_back({{"id", a.id},
                       {"name", a.name},
                       {"transport", a.transport},
                       {"prefix", a.prefix},
                       {"tools", a.tool_names},
                       {"argv", a.argv}});
    }
    return {{"ok", true}, {"impl", "real"}, {"sessionId", session_id}, {"mcp_servers", arr}};
}

nlohmann::json AgentEndpoint::RemoveSessionMcp(const std::string& session_id, const std::string& mcp_id) {
    std::lock_guard lock(mu_);
    auto* s = Find(session_id);
    if (!s) return {{"ok", false}, {"error", "session not found"}, {"sessionId", session_id}};
    for (auto it = s->mcp_servers.begin(); it != s->mcp_servers.end(); ++it) {
        if (it->id == mcp_id || it->name == mcp_id) {
            s->mcp_servers.erase(it);
            return {{"ok", true},
                    {"impl", "real"},
                    {"removed", mcp_id},
                    {"sessionId", session_id},
                    {"mcp_servers", s->mcp_servers.size()}};
        }
    }
    return {{"ok", false}, {"error", "mcp attachment not found"}, {"id", mcp_id}};
}

nlohmann::json AgentEndpoint::NewSession(const nlohmann::json& params) {
    if (AuthRequired() && !Authenticated()) {
        return {{"error", "unauthorized"},
                {"auth_required", true},
                {"hint", "auth/login first"}};
    }
    std::string sid;
    {
        std::lock_guard lock(mu_);
        WireSession s;
        s.id = "sess_" + std::to_string(sess_seq_++);
        s.cwd = params.value("cwd", ".");
        s.status = "idle";
        sessions_.push_back(std::move(s));
        sid = sessions_.back().id;
        Notify("session/update",
               {{"sessionId", sid},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
    }
    nlohmann::json attached = nlohmann::json::array();
    if (params.contains("mcpServers") && params["mcpServers"].is_array()) {
        for (const auto& spec : params["mcpServers"]) {
            if (!spec.is_object()) continue;
            attached.push_back(AddSessionMcp(sid, spec));
        }
    }
    return {{"sessionId", sid},
            {"availableCommands", nlohmann::json::array()},
            {"mcpAttached", attached},
            {"_meta",
             {{"openclawCpp",
               {{"impl", "real"}, {"agent_loop", true}, {"session_mcp", true}}}}}};
}

nlohmann::json AgentEndpoint::RunPromptTurn(std::unique_lock<std::mutex>& lock, WireSession& session,
                                            const PromptParsed& parsed, const std::string& user_mid) {
    EnsureDefaultBackend();
    const std::string sid = session.id;
    auto cancel = session.cancel;
    cancel->store(false);

    agent::Message user_msg;
    user_msg.role = "user";
    user_msg.content = parsed.text;
    if (parsed.images > 0 || parsed.files > 0) {
        user_msg.multimodal_parts = parsed.openai_parts;
    }
    session.history.push_back(std::move(user_msg));
    session.messages.push_back({{"role", "user"},
                                {"messageId", user_mid},
                                {"text", parsed.text},
                                {"images", parsed.images},
                                {"files", parsed.files}});
    session.status = "running";

    auto notify_content = parsed.notify_parts.empty()
                              ? nlohmann::json::array({{{"type", "text"}, {"text", parsed.text}}})
                              : parsed.notify_parts;
    Notify("session/update",
           {{"sessionId", sid},
            {"update",
             {{"sessionUpdate", "user_message"},
              {"messageId", user_mid},
              {"content", notify_content}}}});
    Notify("session/update",
           {{"sessionId", sid},
            {"update", {{"sessionUpdate", "state_update"}, {"state", "running"}}}});

    // Snapshot for loop outside lock
    auto history = session.history;
    auto provider = backend_.provider;
    auto tools = BuildTurnTools(session);
    const std::size_t mcp_tool_count = [&]() {
        std::size_t n = 0;
        for (const auto& a : session.mcp_servers) n += a.tool_names.size();
        return n;
    }();
    const std::size_t mcp_server_count = session.mcp_servers.size();
    auto opts = backend_.opts;
    opts.cancel = cancel.get();
    const std::string provider_name = backend_.provider_name;

    const int delay_ms = TurnDelayMs();
    // Release lock while running the agent loop so session/cancel can set the flag.
    lock.unlock();
    if (delay_ms > 0) {
        const auto slice = std::chrono::milliseconds(10);
        auto left = std::chrono::milliseconds(delay_ms);
        while (left.count() > 0) {
            if (cancel->load()) break;
            auto step = left < slice ? left : slice;
            std::this_thread::sleep_for(step);
            left -= step;
        }
    }

    agent::LoopResult loop_out;
    if (cancel->load()) {
        loop_out.ok = false;
        loop_out.cancelled = true;
        loop_out.error = "cancelled";
    } else if (!provider || !tools) {
        loop_out.ok = false;
        loop_out.error = "loop backend not configured";
    } else {
        loop_out = agent::RunAgentLoop(*provider, *tools, std::move(history), opts);
    }
    lock.lock();

    // Session may have been closed while unlocked
    auto* live = Find(sid);
    if (!live) {
        return {{"messageId", user_mid},
                {"_meta", {{"openclawCpp", {{"warning", "session closed during turn"}}}}}};
    }

    const std::string agent_mid = "msg_agent_" + std::to_string(msg_seq_++);
    std::string reply;
    std::string state = "idle";
    if (loop_out.cancelled) {
        reply = "[cancelled]";
        state = "idle";
        Notify("session/update",
               {{"sessionId", sid},
                {"update",
                 {{"sessionUpdate", "state_update"},
                  {"state", "idle"},
                  {"_meta", {{"cancelled", true}}}}}});
    } else if (!loop_out.ok) {
        reply = loop_out.error.empty() ? "[agent loop error]" : loop_out.error;
    } else {
        reply = loop_out.final_text;
    }

    live->history = loop_out.messages.empty() ? live->history : loop_out.messages;
    live->messages.push_back({{"role", "assistant"}, {"messageId", agent_mid}, {"text", reply}});
    live->status = "idle";
    cancel->store(false);

    Notify("session/update",
           {{"sessionId", sid},
            {"update",
             {{"sessionUpdate", "agent_message"},
              {"messageId", agent_mid},
              {"content", nlohmann::json::array({{{"type", "text"}, {"text", reply}}})}}}});
    if (!loop_out.cancelled) {
        Notify("session/update",
               {{"sessionId", sid},
                {"update", {{"sessionUpdate", "state_update"}, {"state", state}}}});
    }

    return {{"messageId", user_mid},
            {"text", reply},
            {"stopReason", loop_out.cancelled ? "cancelled" : (loop_out.ok ? "end_turn" : "error")},
            {"_meta",
             {{"openclawCpp",
               {{"agentMessageId", agent_mid},
                {"impl", "real"},
                {"agent_loop", true},
                {"provider", provider_name},
                {"turns", loop_out.turns},
                {"cancelled", loop_out.cancelled},
                {"ok", loop_out.ok},
                {"text", reply},
                {"images", parsed.images},
                {"files", parsed.files},
                {"multimodal", parsed.images > 0 || parsed.files > 0},
                {"mcp_servers", mcp_server_count},
                {"mcp_tools", mcp_tool_count},
                {"tools_merged", tools ? tools->Names().size() : 0},
                {"note", "RunAgentLoop result surfaced as session/update agent_message"}}}}}};
}

nlohmann::json AgentEndpoint::Prompt(const std::string& session_id,
                                     const nlohmann::json& prompt_blocks) {
    if (AuthRequired() && !Authenticated()) {
        return {{"error", "unauthorized"},
                {"auth_required", true},
                {"sessionId", session_id},
                {"hint", "auth/login first"}};
    }
    std::unique_lock lock(mu_);
    EnsureDefaultBackend();
    auto* s = Find(session_id);
    if (!s) return {{"error", "session not found"}, {"sessionId", session_id}};
    auto parsed = ParsePrompt(prompt_blocks);
    const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
    return RunPromptTurn(lock, *s, parsed, user_mid);
}

nlohmann::json AgentEndpoint::Handle(const nlohmann::json& request) {
    std::unique_lock lock(mu_);
    if (!request.is_object()) return Err(nullptr, -32600, "Invalid Request");
    const auto id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
    const std::string method = request.value("method", "");
    const auto params = request.value("params", nlohmann::json::object());

    if (!request.contains("id") && !method.empty()) {
        return nlohmann::json();
    }

    if (method == "initialize") {
        int requested = params.value("protocolVersion", 0);
        if (requested < 1) return Err(id, -32602, "Invalid params: protocolVersion");
        if (requested < kAcpProtocolVersion) {
            return Err(id, -32022, "Unsupported protocol version",
                       {{"requested", requested}, {"supported", nlohmann::json::array({kAcpProtocolVersion})}});
        }
        const int selected = kAcpProtocolVersion;
        initialized_ = true;
        client_protocol_ = selected;
        client_info_ = params.value("info", nlohmann::json::object());
        EnsureDefaultBackend();
        // Optional credentials on initialize
        bool authed_now = false;
        if (AuthRequired()) {
            lock.unlock();
            auto login = Login(params);
            lock.lock();
            authed_now = login.value("ok", false) && login.value("authenticated", false);
        } else {
            authenticated_ = true;
            auth_method_ = "none";
            authed_now = true;
        }
        return Ok(id, {{"protocolVersion", selected},
                       {"capabilities", AgentCapabilities()},
                       {"info",
                        {{"name", name_},
                         {"title", "openclaw-cpp ACP agent"},
                         {"version", version_}}},
                       {"authMethods", AuthMethodsJson()},
                       {"_meta",
                        {{"openclawCpp",
                          {{"agent_loop", true},
                           {"provider", backend_.provider_name},
                           {"auth_required", AuthRequired()},
                           {"authenticated", authed_now}}}}}});
    }

    if (method == "ping") return Ok(id, nlohmann::json::object());

    if (method == "auth/login" || method == "auth/authenticate" || method == "authenticate") {
        lock.unlock();
        auto login = Login(params);
        lock.lock();
        if (!login.value("ok", false))
            return Err(id, -32004, login.value("error", "authentication failed"), login);
        return Ok(id, login);
    }

    if (method == "auth/status") {
        lock.unlock();
        auto st = AuthStatusJson();
        lock.lock();
        return Ok(id, st);
    }

    if (method == "auth/logout") {
        authenticated_ = false;
        auth_method_.clear();
        return Ok(id, {{"ok", true}, {"authenticated", false}, {"impl", "real"}});
    }

    if (!initialized_ && method.rfind("session/", 0) == 0) {
        return Err(id, -32002, "Server not initialized");
    }

    if (AuthRequired() && !authenticated_ && !AllowMethodWithoutAuth(method)) {
        return Err(id, -32004, "Unauthorized: ACP auth required",
                   {{"auth_required", true},
                    {"authenticated", false},
                    {"authMethods", AuthMethodsJson()},
                    {"hint", "call auth/login with token or passphrase, or pass authToken on initialize"}});
    }

    if (method == "session/new") {
        WireSession s;
        s.id = "sess_" + std::to_string(sess_seq_++);
        s.cwd = params.value("cwd", ".");
        s.status = "idle";
        sessions_.push_back(std::move(s));
        auto sid = sessions_.back().id;
        Notify("session/update",
               {{"sessionId", sid},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
        nlohmann::json attached = nlohmann::json::array();
        if (params.contains("mcpServers") && params["mcpServers"].is_array()) {
            auto* live = Find(sid);
            if (live) {
                for (const auto& spec : params["mcpServers"]) {
                    if (!spec.is_object()) continue;
                    attached.push_back(AddSessionMcpTo(*live, spec));
                }
            }
        }
        return Ok(id, {{"sessionId", sid},
                       {"availableCommands", nlohmann::json::array()},
                       {"mcpAttached", attached}});
    }

    if (method == "session/mcp/add" || method == "session/mcp.add") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        auto out = AddSessionMcpTo(*s, params.contains("server") ? params["server"] : params);
        if (!out.value("ok", false))
            return Err(id, -32000, out.value("error", "mcp add failed"), out);
        return Ok(id, out);
    }

    if (method == "session/mcp/list" || method == "session/mcp.list") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : s->mcp_servers) {
            arr.push_back({{"id", a.id},
                           {"name", a.name},
                           {"transport", a.transport},
                           {"prefix", a.prefix},
                           {"tools", a.tool_names}});
        }
        return Ok(id, {{"sessionId", sid}, {"mcp_servers", arr}, {"impl", "real"}});
    }

    if (method == "session/mcp/remove" || method == "session/mcp.remove") {
        auto sid = params.value("sessionId", "");
        auto mid = params.value("mcpId", params.value("id", params.value("name", "")));
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        for (auto it = s->mcp_servers.begin(); it != s->mcp_servers.end(); ++it) {
            if (it->id == mid || it->name == mid) {
                s->mcp_servers.erase(it);
                return Ok(id, {{"ok", true},
                               {"removed", mid},
                               {"sessionId", sid},
                               {"mcp_servers", s->mcp_servers.size()}});
            }
        }
        return Err(id, -32001, "MCP attachment not found", {{"mcpId", mid}});
    }

    if (method == "session/list") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions_) {
            arr.push_back({{"sessionId", s.id},
                           {"cwd", s.cwd},
                           {"status", s.status},
                           {"cancel_requested", s.cancel && s.cancel->load()}});
        }
        return Ok(id, {{"sessions", arr}});
    }

    if (method == "session/resume") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        return Ok(id, {{"sessionId", s->id}, {"cwd", s->cwd}, {"status", s->status}});
    }

    if (method == "session/close" || method == "session/delete") {
        auto sid = params.value("sessionId", "");
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->id == sid) {
                if (it->cancel) it->cancel->store(true);
                sessions_.erase(it);
                return Ok(id, {{"closed", true}, {"sessionId", sid}});
            }
        }
        return Err(id, -32001, "Session not found", {{"sessionId", sid}});
    }

    if (method == "session/cancel") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        if (s->cancel) s->cancel->store(true);
        Notify("session/update",
               {{"sessionId", sid},
                {"update",
                 {{"sessionUpdate", "state_update"},
                  {"state", s->status == "running" ? "running" : "idle"},
                  {"_meta", {{"cancel_requested", true}}}}}});
        return Ok(id, {{"cancelled", true}, {"sessionId", sid}, {"cancel_requested", true}});
    }

    if (method == "session/prompt") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        auto prompt = params.contains("prompt") ? params["prompt"] : nlohmann::json::array();
        auto parsed = ParsePrompt(prompt);
        const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
        auto meta = RunPromptTurn(lock, *s, parsed, user_mid);
        // ACP: return messageId of inserted user message; loop outcome in notifications + _meta
        nlohmann::json result{{"messageId", meta.value("messageId", user_mid)}};
        if (meta.contains("_meta")) result["_meta"] = meta["_meta"];
        return Ok(id, result);
    }

    return Err(id, -32601, "Method not found: " + method);
}

Client::Client(WriteLineFn write, ReadLineFn read)
    : write_(std::move(write)), read_(std::move(read)) {}

nlohmann::json Client::Request(const std::string& method, const nlohmann::json& params) {
    last_notifications_.clear();
    const int id = next_id_++;
    nlohmann::json req{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    write_(req.dump());
    for (;;) {
        auto line = read_();
        if (line.empty()) throw std::runtime_error("acp client: EOF");
        auto msg = nlohmann::json::parse(line);
        if (msg.contains("method") && !msg.contains("id")) {
            last_notifications_.push_back(msg);
            continue;
        }
        if (msg.contains("id") && msg["id"] == id) return msg;
        if (msg.contains("method")) last_notifications_.push_back(msg);
    }
}

bool Client::Initialize(const nlohmann::json& client_info) {
    nlohmann::json info = client_info.empty()
                              ? nlohmann::json{{"name", "openclaw-cpp"},
                                               {"title", "openclaw-cpp ACP client"},
                                               {"version", "1.21.0-dev"}}
                              : client_info;
    auto resp = Request("initialize",
                        {{"protocolVersion", kAcpProtocolVersion},
                         {"capabilities", nlohmann::json::object()},
                         {"info", info}});
    return !resp.contains("error") && resp.contains("result");
}

nlohmann::json Client::SessionNew(const std::string& cwd, const nlohmann::json& mcp_servers) {
    return Request("session/new", {{"cwd", cwd}, {"mcpServers", mcp_servers}});
}

nlohmann::json Client::SessionList() { return Request("session/list", nlohmann::json::object()); }

nlohmann::json Client::SessionPrompt(const std::string& session_id, const std::string& text) {
    nlohmann::json blocks = nlohmann::json::array({{{"type", "text"}, {"text", text}}});
    return Request("session/prompt", {{"sessionId", session_id}, {"prompt", blocks}});
}

nlohmann::json Client::SessionPromptParts(const std::string& session_id,
                                          const nlohmann::json& prompt_blocks) {
    return Request("session/prompt", {{"sessionId", session_id}, {"prompt", prompt_blocks}});
}

nlohmann::json Client::SessionCancel(const std::string& session_id) {
    return Request("session/cancel", {{"sessionId", session_id}});
}

nlohmann::json Client::SessionClose(const std::string& session_id) {
    return Request("session/close", {{"sessionId", session_id}});
}

std::pair<std::shared_ptr<Client>, std::shared_ptr<AgentEndpoint>> Client::MakeInProcessPair() {
    auto agent = std::make_shared<AgentEndpoint>();
    // EnsureDefaultBackend is private — initialize via Handle or SetLoopBackend here
    {
        LoopBackend b;
        b.provider = std::make_shared<provider::MockProvider>();
        b.tools = std::make_shared<tools::Registry>();
        b.opts.max_turns = 4;
        b.provider_name = "mock";
        agent->SetLoopBackend(std::move(b));
    }
    auto to_client = std::make_shared<std::queue<std::string>>();
    auto mu = std::make_shared<std::mutex>();

    auto write_agent = [to_client, agent, mu](const std::string& line) {
        // Do NOT hold mu across Handle — Handle unlocks during AgentLoop for cancel.
        auto req = nlohmann::json::parse(line);
        auto resp = agent->Handle(req);
        auto notes = agent->DrainNotifications();
        std::lock_guard lock(*mu);
        for (auto& n : notes) to_client->push(n.dump());
        if (!resp.is_null() && !resp.empty()) to_client->push(resp.dump());
    };
    auto read_client = [to_client, mu]() -> std::string {
        std::lock_guard lock(*mu);
        if (to_client->empty()) return {};
        auto s = to_client->front();
        to_client->pop();
        return s;
    };

    auto client = std::make_shared<Client>(write_agent, read_client);
    return {client, agent};
}

}  // namespace openclaw::acp
