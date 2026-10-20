#include "openclaw/acp/wire.hpp"
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
                 {{"note", "subset — AgentLoop on prompt; text+image(+file) parts; no session MCP / acpx"}}}}}}}};
}

int TurnDelayMs() {
    const char* v = std::getenv("OPENCLAW_ACP_TURN_DELAY_MS");
    if (!v || !*v) return 0;
    return std::atoi(v);
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
            {"note", "session/prompt → RunAgentLoop; multimodal text+image(+file); channel bind; not acpx"}};
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
                       {"cancel_requested", s.cancel && s.cancel->load()}});
    }
    return arr;
}

nlohmann::json AgentEndpoint::NewSession(const nlohmann::json& params) {
    std::lock_guard lock(mu_);
    WireSession s;
    s.id = "sess_" + std::to_string(sess_seq_++);
    s.cwd = params.value("cwd", ".");
    s.status = "idle";
    sessions_.push_back(std::move(s));
    Notify("session/update",
           {{"sessionId", sessions_.back().id},
            {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
    return {{"sessionId", sessions_.back().id},
            {"availableCommands", nlohmann::json::array()},
            {"_meta", {{"openclawCpp", {{"impl", "real"}, {"agent_loop", true}}}}}};
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
    auto tools = backend_.tools;
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
                {"note", "RunAgentLoop result surfaced as session/update agent_message"}}}}}};
}

nlohmann::json AgentEndpoint::Prompt(const std::string& session_id,
                                     const nlohmann::json& prompt_blocks) {
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
        return Ok(id, {{"protocolVersion", selected},
                       {"capabilities", AgentCapabilities()},
                       {"info",
                        {{"name", name_},
                         {"title", "openclaw-cpp ACP agent"},
                         {"version", version_}}},
                       {"authMethods", nlohmann::json::array()},
                       {"_meta",
                        {{"openclawCpp",
                          {{"agent_loop", true}, {"provider", backend_.provider_name}}}}}});
    }

    if (method == "ping") return Ok(id, nlohmann::json::object());

    if (!initialized_ && method.rfind("session/", 0) == 0) {
        return Err(id, -32002, "Server not initialized");
    }

    if (method == "session/new") {
        WireSession s;
        s.id = "sess_" + std::to_string(sess_seq_++);
        s.cwd = params.value("cwd", ".");
        s.status = "idle";
        sessions_.push_back(std::move(s));
        Notify("session/update",
               {{"sessionId", sessions_.back().id},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
        return Ok(id, {{"sessionId", sessions_.back().id}, {"availableCommands", nlohmann::json::array()}});
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
                                               {"version", "1.8.0-dev"}}
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
