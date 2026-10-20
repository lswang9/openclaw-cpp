#include "openclaw/acp/wire.hpp"

#include <queue>
#include <stdexcept>

namespace openclaw::acp {
namespace {

nlohmann::json AgentCapabilities() {
    // Honest subset: session surface with baseline text prompts only (no image/audio/MCP).
    return {{"session",
             {{"prompt", nlohmann::json::object()},  // baseline text + resource-link only
              {"_meta",
               {{"openclawCpp",
                 {{"note", "subset — no multimodal / no session MCP / no acpx"}}}}}}}};
}

}  // namespace

AgentEndpoint::AgentEndpoint(std::string agent_name, std::string agent_version)
    : name_(std::move(agent_name)), version_(std::move(agent_version)) {}

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
            out += "[resource " + b.value("uri", b.value("resource", nlohmann::json::object()).value("uri", "")) + "]";
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
            {"protocolVersion", kAcpProtocolVersion},
            {"initialized", initialized_},
            {"agent", {{"name", name_}, {"version", version_}}},
            {"sessions", sessions_.size()},
            {"capabilities", AgentCapabilities()},
            {"gap_doc", "docs/ACP-GAP.md"},
            {"note", "ACP JSON-RPC subset (stdio/in-process); not full acpx / channel-bound ACP"}};
}

nlohmann::json AgentEndpoint::ListSessionsJson() const {
    std::lock_guard lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions_) {
        arr.push_back({{"sessionId", s.id},
                       {"cwd", s.cwd},
                       {"status", s.status},
                       {"messages", s.messages.size()}});
    }
    return arr;
}

nlohmann::json AgentEndpoint::NewSession(const nlohmann::json& params) {
    std::lock_guard lock(mu_);
    WireSession s;
    s.id = "sess_" + std::to_string(sess_seq_++);
    s.cwd = params.value("cwd", ".");
    s.status = "idle";
    sessions_.push_back(s);
    Notify("session/update",
           {{"sessionId", s.id},
            {"update",
             {{"sessionUpdate", "state_update"}, {"state", "idle"}, {"_meta", {{"openclawCpp", true}}}}}});
    return {{"sessionId", s.id},
            {"availableCommands", nlohmann::json::array()},
            {"_meta", {{"openclawCpp", {{"impl", "real"}, {"note", "in-process/stdio ACP agent subset"}}}}}};
}

nlohmann::json AgentEndpoint::Prompt(const std::string& session_id,
                                     const nlohmann::json& prompt_blocks) {
    std::lock_guard lock(mu_);
    auto* s = Find(session_id);
    if (!s) return {{"error", "session not found"}, {"sessionId", session_id}};
    const std::string text = PromptText(prompt_blocks);
    const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
    const std::string agent_mid = "msg_agent_" + std::to_string(msg_seq_++);
    s->status = "running";
    s->messages.push_back({{"role", "user"}, {"messageId", user_mid}, {"text", text}});

    Notify("session/update",
           {{"sessionId", session_id},
            {"update",
             {{"sessionUpdate", "user_message"},
              {"messageId", user_mid},
              {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}}}});

    // Honest mock agent turn: echo / acknowledge (not a live LLM unless wired later).
    std::string reply = "[openclaw-cpp ACP] received: " + (text.size() > 400 ? text.substr(0, 400) + "…" : text);
    s->messages.push_back({{"role", "assistant"}, {"messageId", agent_mid}, {"text", reply}});
    Notify("session/update",
           {{"sessionId", session_id},
            {"update",
             {{"sessionUpdate", "agent_message"},
              {"messageId", agent_mid},
              {"content", nlohmann::json::array({{{"type", "text"}, {"text", reply}}})}}}});
    Notify("session/update",
           {{"sessionId", session_id},
            {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
    s->status = "idle";
    return {{"messageId", user_mid},
            {"_meta",
             {{"openclawCpp",
               {{"agentMessageId", agent_mid},
                {"impl", "real"},
                {"note", "prompt accepted + mock agent reply via session/update; not upstream LLM harness"}}}}}};
}

nlohmann::json AgentEndpoint::Handle(const nlohmann::json& request) {
    std::lock_guard lock(mu_);
    if (!request.is_object()) return Err(nullptr, -32600, "Invalid Request");
    const auto id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
    const std::string method = request.value("method", "");
    const auto params = request.value("params", nlohmann::json::object());

    // Notifications from client (no response)
    if (!request.contains("id") && !method.empty()) {
        return nlohmann::json();  // empty → caller skips write
    }

    if (method == "initialize") {
        int requested = params.value("protocolVersion", 0);
        if (requested < 1) return Err(id, -32602, "Invalid params: protocolVersion");
        // Advertise/select ACP v2 only (honest: v1 shapes not implemented).
        if (requested < kAcpProtocolVersion) {
            return Err(id, -32022, "Unsupported protocol version",
                       {{"requested", requested}, {"supported", nlohmann::json::array({kAcpProtocolVersion})}});
        }
        const int selected = kAcpProtocolVersion;  // never higher than requested (requested >= 2)
        initialized_ = true;
        client_protocol_ = selected;
        client_info_ = params.value("info", nlohmann::json::object());
        return Ok(id, {{"protocolVersion", selected},
                       {"capabilities", AgentCapabilities()},
                       {"info",
                        {{"name", name_},
                         {"title", "openclaw-cpp ACP agent"},
                         {"version", version_}}},
                       {"authMethods", nlohmann::json::array()}});
    }

    if (method == "ping") return Ok(id, nlohmann::json::object());

    if (!initialized_ && method.rfind("session/", 0) == 0) {
        return Err(id, -32002, "Server not initialized");
    }

    if (method == "session/new") {
        // unlock for NewSession? we're holding mu_ — NewSession also locks. Deadlock!
        // Inline here instead.
        WireSession s;
        s.id = "sess_" + std::to_string(sess_seq_++);
        s.cwd = params.value("cwd", ".");
        s.status = "idle";
        sessions_.push_back(s);
        Notify("session/update",
               {{"sessionId", s.id},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
        return Ok(id, {{"sessionId", s.id}, {"availableCommands", nlohmann::json::array()}});
    }

    if (method == "session/list") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions_) {
            arr.push_back({{"sessionId", s.id}, {"cwd", s.cwd}, {"status", s.status}});
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
        s->status = "idle";
        Notify("session/update",
               {{"sessionId", sid},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
        return Ok(id, {{"cancelled", true}, {"sessionId", sid}});
    }

    if (method == "session/prompt") {
        auto sid = params.value("sessionId", "");
        auto* s = Find(sid);
        if (!s) return Err(id, -32001, "Session not found", {{"sessionId", sid}});
        auto prompt = params.contains("prompt") ? params["prompt"] : nlohmann::json::array();
        // Temporarily unlock pattern: do prompt logic inline (already hold lock)
        const std::string text = PromptText(prompt);
        const std::string user_mid = "msg_user_" + std::to_string(msg_seq_++);
        const std::string agent_mid = "msg_agent_" + std::to_string(msg_seq_++);
        s->status = "running";
        s->messages.push_back({{"role", "user"}, {"messageId", user_mid}, {"text", text}});
        Notify("session/update",
               {{"sessionId", sid},
                {"update",
                 {{"sessionUpdate", "user_message"},
                  {"messageId", user_mid},
                  {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}}}});
        std::string reply =
            "[openclaw-cpp ACP] received: " + (text.size() > 400 ? text.substr(0, 400) + "…" : text);
        s->messages.push_back({{"role", "assistant"}, {"messageId", agent_mid}, {"text", reply}});
        Notify("session/update",
               {{"sessionId", sid},
                {"update",
                 {{"sessionUpdate", "agent_message"},
                  {"messageId", agent_mid},
                  {"content", nlohmann::json::array({{{"type", "text"}, {"text", reply}}})}}}});
        Notify("session/update",
               {{"sessionId", sid},
                {"update", {{"sessionUpdate", "state_update"}, {"state", "idle"}}}});
        s->status = "idle";
        return Ok(id, {{"messageId", user_mid}});
    }

    return Err(id, -32601, "Method not found: " + method);
}

// ---- Client ----

Client::Client(WriteLineFn write, ReadLineFn read)
    : write_(std::move(write)), read_(std::move(read)) {}

nlohmann::json Client::Request(const std::string& method, const nlohmann::json& params) {
    last_notifications_.clear();
    const int id = next_id_++;
    nlohmann::json req{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    write_(req.dump());
    // Read until matching response; collect notifications.
    for (;;) {
        auto line = read_();
        if (line.empty()) throw std::runtime_error("acp client: EOF");
        auto msg = nlohmann::json::parse(line);
        if (msg.contains("method") && !msg.contains("id")) {
            last_notifications_.push_back(msg);
            continue;
        }
        if (msg.contains("id") && msg["id"] == id) return msg;
        // Unexpected response — keep waiting briefly by treating as notify skip
        if (msg.contains("method")) last_notifications_.push_back(msg);
    }
}

bool Client::Initialize(const nlohmann::json& client_info) {
    nlohmann::json info = client_info.empty()
                              ? nlohmann::json{{"name", "openclaw-cpp"},
                                               {"title", "openclaw-cpp ACP client"},
                                               {"version", "1.7.0-dev"}}
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

nlohmann::json Client::SessionClose(const std::string& session_id) {
    return Request("session/close", {{"sessionId", session_id}});
}

std::pair<std::shared_ptr<Client>, std::shared_ptr<AgentEndpoint>> Client::MakeInProcessPair() {
    auto agent = std::make_shared<AgentEndpoint>();
    // Queues for bidirectional lines
    auto to_agent = std::make_shared<std::queue<std::string>>();
    auto to_client = std::make_shared<std::queue<std::string>>();
    auto mu = std::make_shared<std::mutex>();

    auto write_agent = [to_agent, to_client, agent, mu](const std::string& line) {
        // Client → agent: handle immediately and push response + notifications to client queue
        std::lock_guard lock(*mu);
        auto req = nlohmann::json::parse(line);
        auto resp = agent->Handle(req);
        auto notes = agent->DrainNotifications();
        for (auto& n : notes) to_client->push(n.dump());
        if (!resp.is_null() && !resp.empty()) to_client->push(resp.dump());
        (void)to_agent;
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
