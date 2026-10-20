#include "openclaw/acp/wire.hpp"
#include "openclaw/provider/mock.hpp"
#include "openclaw/util.hpp"

#include <chrono>
#include <cstdlib>
#include <queue>
#include <stdexcept>
#include <thread>

namespace openclaw::acp {
namespace {

nlohmann::json AgentCapabilities() {
    return {{"session",
             {{"prompt", nlohmann::json::object()},
              {"_meta",
               {{"openclawCpp",
                 {{"note", "subset — AgentLoop on prompt; no multimodal / session MCP / acpx"}}}}}}}};
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
    if (prompt_blocks.is_string()) return prompt_blocks.get<std::string>();
    if (!prompt_blocks.is_array()) return {};
    std::string out;
    for (const auto& b : prompt_blocks) {
        if (b.is_string()) {
            if (!out.empty()) out += "\n";
            out += b.get<std::string>();
            continue;
        }
        if (!b.is_object()) continue;
        auto type = b.value("type", "");
        if (type == "text" || b.contains("text")) {
            if (!out.empty()) out += "\n";
            out += b.value("text", "");
        } else if (type == "resource_link" || type == "resource") {
            if (!out.empty()) out += "\n";
            out += "[resource " +
                   b.value("uri", b.value("resource", nlohmann::json::object()).value("uri", "")) + "]";
        }
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
            {"note", "session/prompt → RunAgentLoop; default mock provider; not acpx/channel-bound"}};
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
                                            const std::string& text, const std::string& user_mid) {
    EnsureDefaultBackend();
    const std::string sid = session.id;
    auto cancel = session.cancel;
    cancel->store(false);

    session.history.push_back(agent::Message{.role = "user", .content = text});
    session.messages.push_back({{"role", "user"}, {"messageId", user_mid}, {"text", text}});
    session.status = "running";

    Notify("session/update",
           {{"sessionId", sid},
            {"update",
             {{"sessionUpdate", "user_message"},
              {"messageId", user_mid},
              {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}}}});
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
            {"_meta",
             {{"openclawCpp",
               {{"agentMessageId", agent_mid},
                {"impl", "real"},
                {"agent_loop", true},
                {"provider", provider_name},
                {"turns", loop_out.turns},
                {"cancelled", loop_out.cancelled},
                {"ok", loop_out.ok},
                {"note", "RunAgentLoop result surfaced as session/update agent_message"}}}}}};
}

nlohmann::json AgentEndpoint::Prompt(const std::string& session_id,
                                     const nlohmann::json& prompt_blocks) {
    std::unique_lock lock(mu_);
    EnsureDefaultBackend();
    auto* s = Find(session_id);
    if (!s) return {{"error", "session not found"}, {"sessionId", session_id}};
    const std::string text = PromptText(prompt_blocks);
    const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
    return RunPromptTurn(lock, *s, text, user_mid);
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
        const std::string text = PromptText(prompt);
        const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
        auto meta = RunPromptTurn(lock, *s, text, user_mid);
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
