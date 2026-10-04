#include "tools/skill.hpp"
#include "util.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dog {

namespace {

struct Skill {
  std::string name;
  std::string description;
  std::string body;
};

// Parse a SKILL.md with a YAML frontmatter block delimited by `---` lines.
// Only `name:` and `description:` are required; the body is everything after
// the closing delimiter, trimmed.
bool parse_skill(const std::string& path, Skill& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> lines = util::split_lines(content);

  size_t start = std::string::npos;
  for (size_t i = 0; i < lines.size(); ++i)
    if (util::trim(lines[i]) == "---") {
      start = i;
      break;
    }
  if (start == std::string::npos) return false;

  size_t end = std::string::npos;
  for (size_t i = start + 1; i < lines.size(); ++i)
    if (util::trim(lines[i]) == "---") {
      end = i;
      break;
    }
  if (end == std::string::npos) return false;

  std::string name, desc;
  for (size_t i = start + 1; i < end; ++i) {
    std::string t = lines[i];
    while (!t.empty() && (t[0] == ' ' || t[0] == '\t')) t.erase(t.begin());
    if (t.rfind("name:", 0) == 0)
      name = util::trim(t.substr(5));
    else if (t.rfind("description:", 0) == 0)
      desc = util::trim(t.substr(12));
  }
  if (name.empty() || desc.empty()) return false;

  std::string body;
  for (size_t i = end + 1; i < lines.size(); ++i) body += lines[i] + "\n";

  out.name = name;
  out.description = desc;
  out.body = util::trim(body);
  return true;
}

}  // namespace

void register_skills_tools(ToolRegistry& r, const std::string& skills_dir) {
  std::vector<Skill> skills;
  std::error_code ec;
  if (std::filesystem::is_directory(skills_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(skills_dir)) {
      if (!entry.is_directory()) continue;
      Skill s;
      std::string manifest = entry.path().string() + "/SKILL.md";
      if (parse_skill(manifest, s)) skills.push_back(s);
    }
  }
  if (skills.empty()) return;

  std::string desc =
      "Load the full instructions (SKILL.md body) for a named skill, then follow them.\n"
      "Available skills:\n";
  for (const auto& s : skills) desc += "- " + s.name + ": " + s.description + "\n";

  std::vector<Skill> captured = skills;
  r.add("skill",
        desc,
        {{"type", "object"},
         {"properties",
          {{"skill", {{"type", "string"}, {"description", "Name of the skill to load"}}},
           {"args", {{"type", "string"}, {"description", "Optional arguments for the skill"}}}}},
         {"required", {"skill"}},
         {"additionalProperties", false}},
        /*read_only=*/true,
        [captured = std::move(captured)](const nlohmann::json& args) -> std::string {
          std::string name = args.value("skill", "");
          if (name.empty()) return "error: skill is required";
          for (const auto& s : captured)
            if (s.name == name) {
              std::string a = args.value("args", "");
              std::string out = s.body;
              if (!a.empty()) out += "\n\nArguments: " + a;
              return out;
            }
          return "error: unknown skill '" + name + "'";
        });
}

}  // namespace dog
