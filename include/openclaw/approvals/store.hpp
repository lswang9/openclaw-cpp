#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::approvals {

struct Request {
    std::string id;
    std::string kind;  // exec | plugin | openclaw
    std::string summary;
    std::string status;  // pending | approved | rejected
};

class Store {
public:
    std::string Create(const std::string& kind, const std::string& summary);
    bool Resolve(const std::string& id, bool approve);
    std::vector<Request> List(const std::string& status_filter = "") const;
    nlohmann::json ListJson(const std::string& status_filter = "") const;
    nlohmann::json GetJson(const std::string& id) const;

private:
    std::vector<Request> items_;
    int seq_ = 1;
};

}  // namespace openclaw::approvals
