#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dog {

struct McpServerConfig {
  std::string name;
  std::string transport;  // "stdio" | "http"
  std::string command;    // for stdio
  std::vector<std::string> args;
  std::string url;        // for http
};

struct Config {
  std::string base_url;  // e.g. http://localhost:8000/v1
  std::string api_key;
  std::string model;
  long context_window = 128000;  // model context size in tokens (0 = unknown)
  double compact_threshold = 0.75;  // compact when prompt_tokens exceed this fraction of the window
  int compact_keep = 6;             // messages kept verbatim at the tail when compacting
  int max_turns = 500;
  std::string skills_dir = "skills";
  std::string system_file = "SYSTEM.md";
  std::vector<McpServerConfig> mcp;

  static Config load(const std::string& config_path);
  static std::string default_config_path();  // ~/.config/dog/dog.json (XDG aware)
};

}  // namespace dog
