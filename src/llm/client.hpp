#pragma once
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dog {

struct ToolCall {
  std::string id;
  std::string name;
  std::string args_json;  // raw arguments string as streamed by the model
};

struct ChatUsage {
  long prompt_tokens = 0;
  long completion_tokens = 0;
  long total_tokens = 0;
  bool present = false;  // endpoint returned a usage block
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> tool_calls;
  ChatUsage usage;
};

struct LlmConfig {
  std::string base_url;
  std::string api_key;
  std::string model;
};

// One streaming chat completion against an OpenAI-compatible endpoint.
// `messages` and `tools` are OpenAI-format JSON. on_text_chunk is called with
// streamed assistant text as it arrives; on_reasoning_chunk with streamed
// reasoning tokens (reasoning models) when the model emits them. Usage is
// captured into out->usage if the endpoint returns it. Returns false on
// transport/HTTP error (err set).
bool chat(const LlmConfig& cfg,
          const nlohmann::json& messages,
          const nlohmann::json& tools,
          std::function<void(const std::string&)> on_text_chunk,
          std::function<void(const std::string&)> on_reasoning_chunk,
          ChatResult* out,
          std::string* err);

}  // namespace dog
