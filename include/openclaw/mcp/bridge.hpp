#pragma once

#include "openclaw/mcp/client.hpp"
#include "openclaw/tools/registry.hpp"

#include <memory>
#include <string>

namespace openclaw::mcp {

// Register remote MCP tools into a local tools::Registry as "mcp.<name>".
// Client must outlive the registry handlers (shared_ptr recommended).
std::size_t RegisterMcpTools(tools::Registry& registry, std::shared_ptr<Client> client,
                             const std::string& prefix = "mcp.");

// Build an in-process mock MCP client (uses MockServerHandle).
std::shared_ptr<Client> MakeMockClient();

}  // namespace openclaw::mcp
