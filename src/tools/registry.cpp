#include "tools/registry.hpp"

namespace dog {

void ToolRegistry::add(std::string name, std::string description, nlohmann::json params_schema,
                       bool read_only, Executor exec) {
  entries_.push_back({std::move(name), std::move(description), std::move(params_schema),
                       read_only, std::move(exec)});
}

nlohmann::json ToolRegistry::tools_array(bool read_only_only) const {
  nlohmann::json arr = nlohmann::json::array();
  for (auto& e : entries_) {
    if (read_only_only && !e.read_only) continue;
    nlohmann::json fn = {{"name", e.name},
                         {"description", e.description},
                         {"parameters", e.params}};
    arr.push_back({{"type", "function"}, {"function", fn}});
  }
  return arr;
}

std::string ToolRegistry::dispatch(const std::string& name, const nlohmann::json& args) const {
  for (auto& e : entries_) {
    if (e.name == name) return e.exec(args);
  }
  return "error: unknown tool: " + name;
}

bool ToolRegistry::known(const std::string& name) const {
  for (auto& e : entries_)
    if (e.name == name) return true;
  return false;
}

bool ToolRegistry::is_read_only(const std::string& name) const {
  for (auto& e : entries_)
    if (e.name == name) return e.read_only;
  return false;
}

}  // namespace dog
