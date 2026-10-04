#pragma once
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dog {

// A registry of tools. Each tool is an OpenAI function schema plus an executor.
// `read_only` tools are the only ones allowed to run while in plan mode.
class ToolRegistry {
public:
  using Executor = std::function<std::string(const nlohmann::json&)>;

  void add(std::string name, std::string description, nlohmann::json params_schema,
           bool read_only, Executor exec);

  // Build the OpenAI "tools" array. When read_only_only is true, only read_only
  // tools are included (used in plan mode).
  nlohmann::json tools_array(bool read_only_only) const;

  // Execute a tool by name. Returns the result string, or an error string.
  std::string dispatch(const std::string& name, const nlohmann::json& args) const;

  bool known(const std::string& name) const;
  bool is_read_only(const std::string& name) const;

private:
  struct Entry {
    std::string name;
    std::string description;
    nlohmann::json params;
    bool read_only;
    Executor exec;
  };
  std::vector<Entry> entries_;
};

}  // namespace dog
