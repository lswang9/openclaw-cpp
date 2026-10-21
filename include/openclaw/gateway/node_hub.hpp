#pragma once
// Node host protocol (upstream v2026.10.5 shape): a node connects over the gateway WebSocket, sends
// `connect` with role "node" (+ client id/displayName, caps, commands), receives `node.invoke.request` events
// ({id, nodeId, command, paramsJSON, timeoutMs, idempotencyKey}) and answers with the `node.invoke.result` RPC
// ({id, nodeId, ok, payloadJSON | error}). Operators reach nodes with `node.invoke`.
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace openclaw::gateway {

class NodeHub {
public:
    using SendFn = std::function<bool(const std::string&)>;
    static NodeHub& Global();
    // Returns the registered node id ("" + *err on invalid connect params).
    std::string Attach(int conn, const nlohmann::json& connect_params, SendFn send, std::string* err);
    // Returns the node id that went offline ("" when conn was not a node).
    std::string Detach(int conn);
    bool Connected(const std::string& node_id) const;
    std::string NodeOf(int conn) const;  // "" when conn is not a node connection
    bool HasCommand(const std::string& node_id, const std::string& command) const;
    nlohmann::json Describe(const std::string& node_id) const;  // null when unknown
    nlohmann::json List() const;
    // Blocks until the node answers or timeout. On success returns {ok:true, payload, payloadJSON}; else {ok:false, error{code,message}}.
    nlohmann::json Invoke(const std::string& node_id, const std::string& command, const nlohmann::json& params,
                          int timeout_ms, const std::string& idempotency_key = {});
    // Called for node.invoke.result arriving on connection `conn`.
    bool Result(int conn, const nlohmann::json& params, std::string* err);
};

}  // namespace openclaw::gateway
