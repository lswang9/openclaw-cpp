#include "openclaw/approvals/store.hpp"

namespace openclaw::approvals {

std::string Store::Create(const std::string& kind, const std::string& summary) {
    Request r;
    r.id = "appr-" + std::to_string(seq_++);
    r.kind = kind;
    r.summary = summary;
    r.status = "pending";
    items_.push_back(r);
    return r.id;
}

bool Store::Resolve(const std::string& id, bool approve) {
    for (auto& r : items_) {
        if (r.id == id && r.status == "pending") {
            r.status = approve ? "approved" : "rejected";
            return true;
        }
    }
    return false;
}

std::vector<Request> Store::List(const std::string& status_filter) const {
    std::vector<Request> out;
    for (const auto& r : items_) {
        if (status_filter.empty() || r.status == status_filter) {
            out.push_back(r);
        }
    }
    return out;
}

nlohmann::json Store::ListJson(const std::string& status_filter) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : List(status_filter)) {
        arr.push_back({{"id", r.id}, {"kind", r.kind}, {"summary", r.summary}, {"status", r.status}});
    }
    return arr;
}

nlohmann::json Store::GetJson(const std::string& id) const {
    for (const auto& r : items_) {
        if (r.id == id) {
            return {{"id", r.id}, {"kind", r.kind}, {"summary", r.summary}, {"status", r.status}};
        }
    }
    return nlohmann::json{{"error", "not found"}, {"id", id}};
}

}  // namespace openclaw::approvals
