#pragma once
#include <string>
#include "tools/registry.hpp"

namespace dog {
// Scan skills_dir/<skill>/SKILL.md and register a single read_only "skill" tool
// that returns the selected skill's SKILL.md body. No-op if no skills are found.
void register_skills_tools(ToolRegistry& r, const std::string& skills_dir);
}  // namespace dog
