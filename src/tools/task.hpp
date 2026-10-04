#pragma once
#include <functional>
#include <string>
#include "config/config.hpp"
#include "tools/registry.hpp"

namespace dog {
// Register a `task` tool: delegates a self-contained subtask to a nested subagent
// (its own model loop + core tools, no MCP, no task) and returns its final answer.
void register_task_tool(ToolRegistry& r, const Config& cfg, const std::string& skills_dir,
                        std::function<void(const std::string&)> log = {});
}  // namespace dog
