#pragma once
// Slack interactive replies: reply directives -> Block Kit, and block_actions /
// view_submission / shortcut payloads -> agent inbound text.
#include "openclaw/channels/registry.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::channels::slack_interactive {

// Parses reply directives:
//   [[buttons: Label|value, Label2|value2]]
//   [[select: Placeholder|opt1, opt2]]
//   [[modal: Title|Field one, Field two]]   (button that opens a modal)
// Returns {"text": <reply without directives>, "blocks": [...] (only if any directive)}.
nlohmann::json RenderReply(const std::string& reply);

// Modal view (views.open) with one plain_text_input per field.
nlohmann::json BuildModalView(const std::string& title, const std::vector<std::string>& fields,
                              const std::string& callback_id = "openclaw_modal");

struct Interaction {
    std::optional<InboundMessage> inbound;  // message for the agent (if any)
    nlohmann::json api_call;                // e.g. {"method":"views.open","body":{...}} (null if none)
    std::string response_url;
    std::string kind;                       // block_actions | view_submission | shortcut | ...
    bool ack_clear_errors = false;
};

// payload: the parsed interactive payload (Slack sends it as form field `payload`).
Interaction Parse(const nlohmann::json& payload);

// True if the body (HTTP JSON / form-decoded) is an interactive payload; normalises into *out.
bool ExtractPayload(const nlohmann::json& body, nlohmann::json* out);

}  // namespace openclaw::channels::slack_interactive
