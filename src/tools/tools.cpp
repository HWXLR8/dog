#include "tools/tools.hpp"
#include "tools/fs.hpp"
#include "tools/search.hpp"
#include "tools/skill.hpp"
#include "tools/shell.hpp"
#include "tools/web.hpp"

namespace dog {

void register_core_tools(ToolRegistry& r, const std::string& skills_dir) {
  register_fs_tools(r);
  register_shell_tools(r);
  register_search_tools(r);
  register_web_tools(r);
  register_skills_tools(r, skills_dir);
}

}  // namespace dog
