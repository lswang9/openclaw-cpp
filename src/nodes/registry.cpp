#include "openclaw/nodes/registry.hpp"
#include <algorithm>

namespace openclaw::nodes {

void Registry::Upsert(Node n) {
    for (auto& x : nodes_) {
        if (x.id == n.id) { x = std::move(n); return; }
    }
    nodes_.push_back(std::move(n));
}

bool Registry::Remove(const std::string& id) {
    const auto sz = nodes_.size();
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const Node& n) { return n.id == id; }),
                 nodes_.end());
    return nodes_.size() != sz;
}

std::vector<Node> Registry::List() const { return nodes_; }

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : nodes_) {
        arr.push_back({{"id", n.id}, {"name", n.name}, {"kind", n.kind}, {"online", n.online}});
    }
    return arr;
}

nlohmann::json Registry::Describe(const std::string& id) const {
    for (const auto& n : nodes_) {
        if (n.id == id) {
            return {{"id", n.id}, {"name", n.name}, {"kind", n.kind}, {"online", n.online},
                    {"capabilities", nlohmann::json::array({"ping", "status"})}};
        }
    }
    return {{"error", "not found"}, {"id", id}};
}

Registry Registry::MakeDefault() {
    Registry r;
    r.Upsert({.id = "local", .name = "openclaw-cpp-host", .kind = "desktop", .online = true});
    r.Upsert({.id = "node-stub", .name = "companion-stub", .kind = "stub", .online = false});
    return r;
}

}  // namespace openclaw::nodes
