#include "config/config.hpp"
#include <cstdlib>
#include <fstream>

namespace dog {

namespace {
std::string env_or(const char* k, const std::string& d) {
  const char* v = std::getenv(k);
  return (v && *v) ? std::string(v) : d;
}
// ~/.config/dog, honoring XDG_CONFIG_HOME.
std::string config_dir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const std::string base = (xdg && *xdg) ? xdg : (env_or("HOME", "") + "/.config");
  return base + "/dog";
}
}  // namespace

std::string Config::default_config_path() {
  return config_dir() + "/dog.json";
}

Config Config::load(const std::string& config_path) {
  Config c;
  c.base_url = env_or("OPENAI_BASE_URL", "http://localhost:8000/v1");
  c.api_key = env_or("OPENAI_API_KEY", "");
  c.model = env_or("OPENAI_MODEL", "local-model");
  c.skills_dir = env_or("HARNESS_SKILLS_DIR", config_dir() + "/skills");
  c.system_file = env_or("HARNESS_SYSTEM_FILE", config_dir() + "/SYSTEM.md");
  {
    const char* cw = std::getenv("HARNESS_CONTEXT_WINDOW");
    if (cw && *cw) c.context_window = std::atol(cw);
  }
  if (const char* mt = std::getenv("HARNESS_MAX_TURNS"); mt && *mt)
    c.max_turns = std::atoi(mt);
  if (const char* ct = std::getenv("HARNESS_COMPACT_THRESHOLD"); ct && *ct)
    c.compact_threshold = std::atof(ct);
  if (const char* ck = std::getenv("HARNESS_COMPACT_KEEP"); ck && *ck)
    c.compact_keep = std::atoi(ck);

  std::ifstream f(config_path);
  if (f.good()) {
    nlohmann::json j;
    try {
      f >> j;
    } catch (...) {
      return c;
    }
    if (j.contains("model") && j["model"].is_object()) {
      auto& m = j["model"];
      if (m.contains("base_url")) c.base_url = m["base_url"];
      if (m.contains("api_key")) c.api_key = m["api_key"];
      if (m.contains("model")) c.model = m["model"];
      if (m.contains("max_turns")) c.max_turns = m["max_turns"];
      if (m.contains("context_window")) c.context_window = m["context_window"];
      if (m.contains("compact_threshold")) c.compact_threshold = m["compact_threshold"];
      if (m.contains("compact_keep")) c.compact_keep = m["compact_keep"];
    }
    if (j.contains("skills_dir")) c.skills_dir = j["skills_dir"];
    if (j.contains("system_file")) c.system_file = j["system_file"];
    if (j.contains("mcp") && j["mcp"].is_array()) {
      for (auto& s : j["mcp"]) {
        McpServerConfig mc;
        if (s.contains("name")) mc.name = s["name"];
        if (s.contains("transport")) mc.transport = s["transport"];
        if (s.contains("command")) mc.command = s["command"];
        if (s.contains("args") && s["args"].is_array())
          for (auto& a : s["args"]) mc.args.push_back(a.get<std::string>());
        if (s.contains("url")) mc.url = s["url"];
        c.mcp.push_back(std::move(mc));
      }
    }
  }
  return c;
}

}  // namespace dog
