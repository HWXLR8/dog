#pragma once
#include <string>
#include "tools/registry.hpp"

namespace dog {
// Register all built-in tools (fs, shell, search, web_fetch, skills).
void register_core_tools(ToolRegistry& r, const std::string& skills_dir);
}  // namespace dog
