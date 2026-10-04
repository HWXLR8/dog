#include "agent/loop.hpp"
#include "util.hpp"
#include <algorithm>

namespace dog {

namespace {

// Extract the first image part encoded in a tool result as
// "[image:<mime>]<base64>" (emitted by read_file / web_fetch for images).
// Returns true and sets `out` when an image was found; the base64 payload is
// base64-decoded back to raw bytes so the wire part carries a clean data: URI.
inline std::string base64_decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(in.size() * 3 / 4);
  int acc = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    int v = val(c);
    if (v < 0) continue;  // stray char (e.g. a stray space); skip
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += (char)((acc >> bits) & 0xFF);
    }
  }
  return out;
}

bool image_from_result(const std::string& result, ImagePart* out) {
  const char* tag = "[image:";
  if (result.compare(0, 7, tag) != 0) return false;
  size_t colon = result.find(':', 1);
  if (colon == std::string::npos) return false;
  size_t close = result.find(']', colon);
  if (close == std::string::npos) return false;
  std::string mime = result.substr(colon + 1, close - colon - 1);
  std::string b64 = result.substr(close + 1);
  out->mime = mime;
  out->data = base64_decode(b64);
  return !out->data.empty();
}

}  // namespace

nlohmann::json content_or_text(const std::string& text,
                               const std::vector<ImagePart>& images) {
  if (images.empty()) return nlohmann::json(text);
  nlohmann::json parts = nlohmann::json::array();
  if (!text.empty())
    parts.push_back({{"type", "text"}, {"text", text}});
  for (auto& im : images) {
    std::string url = "data:" + im.mime + ";base64," + util::base64_encode(im.data);
    parts.push_back({{"type", "image_url"},
                     {"image_url", {{"url", url}}}});
  }
  return parts;
}

namespace {

const char* kPlanModeRules =
    "[Plan mode is ON] You are in PLAN MODE. Investigate using ONLY read-only tools "
    "(read_file, glob, grep). Do NOT modify files or run state-changing commands. "
    "Form a concrete implementation plan. When ready, call exit_plan_mode with your "
    "plan in the \"plan\" argument. If the user rejects it, revise and call again.";

nlohmann::json enter_plan_mode_schema() {
  nlohmann::json params = {
      {"type", "object"},
      {"properties", nlohmann::json::object()},
      {"additionalProperties", false},
  };
  return {
      {"type", "function"},
      {"function", {
          {"name", "enter_plan_mode"},
          {"description",
           "Switch into plan mode (read-only investigation). Use when the task is "
           "complex or ambiguous and you want to plan before changing anything. "
           "Simple tasks do not need it."},
          {"parameters", params},
      }},
  };
}

nlohmann::json exit_plan_mode_schema() {
  nlohmann::json params = {
      {"type", "object"},
      {"properties",
       {{"plan", {{"type", "string"},
                  {"description", "The plan to present to the user for approval. "
                                   "Concise but concrete (markdown)."}}}}},
      {"required", {"plan"}},
      {"additionalProperties", false},
  };
  return {
      {"type", "function"},
      {"function", {
          {"name", "exit_plan_mode"},
          {"description",
           "You are in plan mode and have finished investigating. Present your "
           "concrete implementation plan here for user approval so you can begin "
           "coding."},
          {"parameters", params},
      }},
  };
}

}  // namespace

AgentLoop::AgentLoop(Config cfg, ToolRegistry* reg, std::string system_prompt,
                     bool start_in_plan, UiCallbacks callbacks)
    : cfg_(std::move(cfg)), reg_(reg), base_system_(std::move(system_prompt)),
      messages_(nlohmann::json::array()), ui_(std::move(callbacks)),
      plan_mode_(start_in_plan) {
  messages_.push_back({{"role", "system"}, {"content", current_system_message()}});
}

std::string AgentLoop::current_system_message() const {
  std::string sys = base_system_;
  if (plan_mode_) sys += "\n\n" + std::string(kPlanModeRules);
  return sys;
}

void AgentLoop::sync_system_message() {
  if (!messages_.empty()) messages_[0]["content"] = current_system_message();
}

namespace {
// Start index of a "safe tail" of up to `keep_last` trailing messages that does not
// split an assistant(tool_calls) message from the tool results that answer it. Index
// 0 is the system message and is never part of the tail.
int safe_tail_start(const nlohmann::json& msgs, int keep_last) {
  int n = (int)msgs.size();
  int idx = std::max(1, n - std::max(0, keep_last));
  while (idx > 1) {
    // If msgs[idx] is a tool result whose parent assistant (msgs[idx-1]) is carrying
    // the tool_calls, pull the boundary back so the pair stays together.
    if (msgs[idx].value("role", std::string()) == "tool" &&
        msgs[idx - 1].value("role", std::string()) == "assistant")
      idx--;
    else
      break;
  }
  return idx;
}
}  // namespace

void AgentLoop::maybe_compact() {
  long window = cfg_.context_window;
  if (window <= 0 || last_prompt_tokens_ <= 0) return;
  long threshold = (long)((double)window * cfg_.compact_threshold);
  if (last_prompt_tokens_ <= threshold) return;

  int tail = safe_tail_start(messages_, cfg_.compact_keep);
  if (tail <= 1) return;  // only the system message + tail: nothing older to fold

  long before = last_prompt_tokens_;
  if (ui_.status) ui_.status("\x1b[38;5;39mcompacting context\u2026\x1b[0m");

  // Summarize messages[1..tail) into a single note. Non-streaming single call.
  nlohmann::json compact_msgs = nlohmann::json::array();
  compact_msgs.push_back(
      {{"role", "system"},
       {"content",
        "Summarize this conversation into a few terse bullet points so the agent can "
        "continue. Keep: decisions made, files/paths changed, key code or API details, "
        "constraints the user stated, and pending/next steps. Drop: tool-call mechanics, "
        "intermediate reasoning, and verbose tool output."}});
  for (size_t i = 1; i < (size_t)tail; i++) {
    nlohmann::json m = messages_[i];
    // Slim: replace any image parts with a text placeholder so the summary
    // side-call doesn't carry megabytes of base64 (it can't "see" them anyway).
    if (m.contains("content") && m["content"].is_array()) {
      nlohmann::json parts = nlohmann::json::array();
      for (auto& p : m["content"]) {
        if (p.contains("type") && p.value("type", "") == "image_url")
          parts.push_back({{"type", "text"}, {"text", "[image omitted]"}});
        else
          parts.push_back(p);
      }
      m["content"] = std::move(parts);
    }
    compact_msgs.push_back(std::move(m));
  }

  LlmConfig llm{cfg_.base_url, cfg_.api_key, cfg_.model};
  ChatResult res;
  std::string err;
  auto noop = [](const std::string&) {};
  bool ok = chat(llm, compact_msgs, nlohmann::json::array(), noop, noop, &res, &err);

  if (ui_.status) ui_.status("");  // clear the transient "compacting" line
  if (!ok || util::trim(res.content).empty()) {
    if (ui_.system) ui_.system("(compaction skipped: " + err + ")");
    return;  // fail open: keep the full history; the threshold re-triggers next round
  }

  nlohmann::json new_msgs = nlohmann::json::array();
  new_msgs.push_back(messages_[0]);  // system prompt, untouched
  new_msgs.push_back(
      {{"role", "system"}, {"content", "[Earlier conversation summary]\n" + res.content}});
  for (size_t i = (size_t)tail; i < messages_.size(); i++) new_msgs.push_back(messages_[i]);
  messages_ = std::move(new_msgs);

  if (ui_.system) ui_.system("context compacted (~" + std::to_string(before) + " tokens folded " +
                             "to " + std::to_string(messages_.size()) + " messages)");
  // Reset so the next real round's prompt_tokens becomes the new baseline.
  last_prompt_tokens_ = 0;
}

nlohmann::json AgentLoop::tools_for_model() const {
  nlohmann::json arr = reg_->tools_array(plan_mode_);
  if (plan_mode_)
    arr.push_back(exit_plan_mode_schema());
  else
    arr.push_back(enter_plan_mode_schema());
  return arr;
}

void AgentLoop::enter_plan_mode() {
  if (plan_mode_) return;
  plan_mode_ = true;
  sync_system_message();
  if (ui_.system) ui_.system("plan mode ON");
}

void AgentLoop::exit_plan_mode() {
  if (!plan_mode_) return;
  plan_mode_ = false;
  sync_system_message();
  if (ui_.system) ui_.system("plan mode OFF");
}

std::string AgentLoop::do_exit_plan_mode(const ToolCall& tc) {
  nlohmann::json args;
  try {
    args = nlohmann::json::parse(tc.args_json);
  } catch (...) {
    args = nlohmann::json::object();
  }
  std::string plan = args.value("plan", "");
  if (util::trim(plan).empty())
    return "error: exit_plan_mode requires a non-empty 'plan' argument.";
  bool approved = true;  // no confirm surface (headless) -> auto-approve
  if (ui_.confirm) approved = ui_.confirm(plan);
  if (approved) {
    exit_plan_mode();
    return "User approved. You can now start implementing the plan.";
  }
  return "The user rejected the plan. Revise it and call exit_plan_mode again.";
}

std::string AgentLoop::dispatch_tool(const ToolCall& tc) {
  if (tc.name == "enter_plan_mode") {
    enter_plan_mode();
    return "Entered plan mode.";
  }
  if (tc.name == "exit_plan_mode") return do_exit_plan_mode(tc);
  if (!reg_->known(tc.name)) return "error: unknown tool: " + tc.name;
  if (plan_mode_ && !reg_->is_read_only(tc.name))
    return "error: tool '" + tc.name + "' is blocked in plan mode (read-only only)";
  nlohmann::json args;
  try {
    args = nlohmann::json::parse(tc.args_json);
  } catch (...) {
    args = nlohmann::json::object();
  }
  return reg_->dispatch(tc.name, args);
}

void AgentLoop::clear_context() {
  messages_ = nlohmann::json::array();
  messages_.push_back({{"role", "system"}, {"content", current_system_message()}});
}

void AgentLoop::stop() {
  // The global flag is what the tools actually observe (they may not share this
  // object, e.g. a subagent running its own AgentLoop).
  interrupt_flag.store(true);
}

std::string AgentLoop::last_assistant_text() const {
  for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
    if (it->value("role", "") == "assistant" && it->contains("content") &&
        (*it)["content"].is_string())
      return (*it)["content"].get<std::string>();
  }
  return "";
}

bool AgentLoop::run_turn(const std::string& user_input,
                         const std::vector<ImagePart>* images) {
  messages_.push_back({{"role", "user"},
                       {"content", content_or_text(util::sanitize_binary(user_input),
                                                   images ? *images : std::vector<ImagePart>{})}});
  LlmConfig llm{cfg_.base_url, cfg_.api_key, cfg_.model};

  for (int turn = 0; turn < cfg_.max_turns; ++turn) {
    if (interrupt_flag.load()) {
      if (ui_.round_end) ui_.round_end();
      if (ui_.system) ui_.system("(interrupted)");
      return false;
    }
    nlohmann::json tools = tools_for_model();
    ChatResult res;
    std::string err;
    if (ui_.model_start) ui_.model_start();
    maybe_compact();  // after model_start so its "compacting" status isn't cleared
    auto on_text = [this](const std::string& t) {
      if (ui_.text) ui_.text(t);
    };
    auto on_reasoning = [this](const std::string& r) {
      if (ui_.reasoning) ui_.reasoning(r);
    };
    bool ok = chat(llm, messages_, tools, on_text, on_reasoning, &res, &err);

    // Interrupted while the model was streaming (curl aborts the transfer).
    // Flush whatever partial output streamed, then stop cleanly.
    if (interrupt_flag.load()) {
      if (ui_.round_end) ui_.round_end();
      if (ui_.system) ui_.system("(interrupted)");
      return false;
    }

    if (!ok) {
      if (ui_.system) ui_.system("ERROR: " + err);
      return false;
    }

    if (ui_.round_end) ui_.round_end();
    if (res.usage.present)
      last_prompt_tokens_ = res.usage.prompt_tokens;
    if (ui_.usage && res.usage.present)
      ui_.usage(res.usage.prompt_tokens, res.usage.completion_tokens, res.usage.total_tokens);

    nlohmann::json assistant;
    assistant["role"] = "assistant";
    if (res.content.empty())
      assistant["content"] = nullptr;
    else
      assistant["content"] = res.content;
    if (!res.tool_calls.empty()) {
      nlohmann::json tcs = nlohmann::json::array();
      for (auto& tc : res.tool_calls) {
        nlohmann::json f = {{"name", tc.name}, {"arguments", tc.args_json}};
        tcs.push_back({{"id", tc.id}, {"type", "function"}, {"function", f}});
      }
      assistant["tool_calls"] = tcs;
    }
    messages_.push_back(assistant);

    if (res.tool_calls.empty()) return true;  // final answer, nothing to run

    // Collect any images the tools returned (e.g. read_file on an image).
    // OpenAI requires `role:"tool"` messages to be text-only, so images are
    // emitted as a single follow-up `role:"user"` message after all tool results
    // stay contiguous (mirrors qwen's splitToolMedia behavior).
    std::vector<ImagePart> pending_images;
    for (auto& tc : res.tool_calls) {
      if (interrupt_flag.load()) {
        if (ui_.tool_call) ui_.tool_call(tc.name, tc.args_json, "(skipped)");
        if (ui_.system) ui_.system("(interrupted)");
        return false;
      }
      std::string result = util::sanitize_binary(dispatch_tool(tc));
      if (ui_.tool_call) ui_.tool_call(tc.name, tc.args_json, result);
      ImagePart im;
      if (image_from_result(result, &im)) pending_images.push_back(std::move(im));
      messages_.push_back({{"role", "tool"}, {"tool_call_id", tc.id}, {"content", result}});
    }
    if (!pending_images.empty()) {
      if (ui_.images) ui_.images(pending_images);
      messages_.push_back({{"role", "user"},
                           {"content",
                            content_or_text("(attached media from previous tool call)",
                                            pending_images)}});
    }
  }
  if (ui_.system) ui_.system("(stopped: reached max turns)");
  return true;
}

}  // namespace dog
