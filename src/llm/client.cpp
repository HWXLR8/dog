#include "llm/client.hpp"
#include "llm/http.hpp"
#include <map>

namespace dog {

namespace {

struct SseAcc {
  std::string buf;
  ChatResult* out;
  std::function<void(const std::string&)>* on_text;
  std::function<void(const std::string&)>* on_reasoning;
  std::map<int, std::string> tc_args;  // index -> accumulated arguments
};

void process_data_line(const std::string& payload, SseAcc* a) {
  if (payload == "[DONE]") return;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(payload);
  } catch (...) {
    return;
  }
  if (j.contains("usage") && j["usage"].is_object()) {
    auto& u = j["usage"];
    auto& out = *a->out;
    out.usage.prompt_tokens = u.value("prompt_tokens", 0L);
    out.usage.completion_tokens = u.value("completion_tokens", 0L);
    out.usage.total_tokens = u.value("total_tokens", 0L);
    if (out.usage.total_tokens > 0 || out.usage.prompt_tokens > 0 ||
        out.usage.completion_tokens > 0)
      out.usage.present = true;
  }
  if (!j.contains("choices") || !j["choices"].is_array()) return;
  for (auto& ch : j["choices"]) {
    if (!ch.contains("delta")) continue;
    auto& d = ch["delta"];
    if (d.contains("content") && d["content"].is_string()) {
      std::string c = d["content"].get<std::string>();
      if (!c.empty()) {
        a->out->content += c;
        if (a->on_text) (*a->on_text)(c);
      }
    }
    if (d.contains("reasoning") && d["reasoning"].is_string()) {
      std::string r = d["reasoning"].get<std::string>();
      if (!r.empty() && a->on_reasoning) (*a->on_reasoning)(r);
    }
    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
      for (auto& tc : d["tool_calls"]) {
        int idx = tc.value("index", 0);
        if (idx < 0) idx = 0;
        if (a->out->tool_calls.size() <= (size_t)idx)
          a->out->tool_calls.resize(idx + 1);
        auto& slot = a->out->tool_calls[idx];
        if (tc.contains("id") && tc["id"].is_string() &&
            !tc["id"].get<std::string>().empty())
          slot.id = tc["id"].get<std::string>();
        if (tc.contains("function")) {
          auto& f = tc["function"];
          if (f.contains("name") && f["name"].is_string())
            slot.name = f["name"].get<std::string>();
          if (f.contains("arguments")) {
            std::string part;
            if (f["arguments"].is_string())
              part = f["arguments"].get<std::string>();
            else
              part = f["arguments"].dump();
            a->tc_args[idx] += part;
            slot.args_json = a->tc_args[idx];
          }
        }
      }
    }
  }
}

}  // namespace

bool chat(const LlmConfig& cfg,
          const nlohmann::json& messages,
          const nlohmann::json& tools,
          std::function<void(const std::string&)> on_text_chunk,
          std::function<void(const std::string&)> on_reasoning_chunk,
          ChatResult* out,
          std::string* err) {
  nlohmann::json req = {{"model", cfg.model},
                        {"messages", messages},
                        {"stream", true},
                        {"stream_options", {{"include_usage", true}}}};
  if (!tools.empty()) req["tools"] = tools;

  std::map<std::string, std::string> headers = {
      {"Content-Type", "application/json"}};
  if (!cfg.api_key.empty()) headers["Authorization"] = "Bearer " + cfg.api_key;

  SseAcc acc;
  acc.out = out;
  acc.on_text = &on_text_chunk;
  acc.on_reasoning = &on_reasoning_chunk;

  auto on_chunk = [&](const char* p, size_t n) {
    acc.buf.append(p, n);
    size_t pos;
    while ((pos = acc.buf.find('\n')) != std::string::npos) {
      std::string line = acc.buf.substr(0, pos);
      acc.buf.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) == 0) {
        std::string payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        process_data_line(payload, &acc);
      }
    }
  };

  std::string body_err;
  int status = http_post(cfg.base_url + "/chat/completions", headers, req.dump(), on_chunk,
                         &body_err);
  if (status == 0) {
    *err = "HTTP transport error: " + body_err;
    return false;
  }
  if (status < 200 || status >= 300) {
    *err = "HTTP " + std::to_string(status) + " from model endpoint";
    return false;
  }
  return true;
}

}  // namespace dog
