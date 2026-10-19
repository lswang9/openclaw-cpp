#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::nodes {

struct Node {
    std::string id;
    std::string name;
    std::string kind;  // desktop | ios | android | stub
    bool online = false;
};

class Registry {
public:
    void Upsert(Node n);
    bool Remove(const std::string& id);
    std::vector<Node> List() const;
    nlohmann::json ListJson() const;
    nlohmann::json Describe(const std::string& id) const;
    static Registry MakeDefault();

private:
    std::vector<Node> nodes_;
};

}  // namespace openclaw::nodes
