#pragma once
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "config/config.hpp"
#include "llm/client.hpp"
#include "tools/registry.hpp"

namespace dog {

// An image to be sent to the model as an OpenAI `image_url` content part.
// `data` is raw image bytes (PNG/JPEG/...); the harness base64-encodes it.
struct ImagePart {
  std::string mime;  // e.g. "image/png"
  std::string data;  // raw bytes
  std::string path;  // source path, for display
};

// Builds the OpenAI `content` array for a message: a (optional) text part plus
// image parts. Returns a single string when there are no images (common case).
nlohmann::json content_or_text(const std::string& text,
                               const std::vector<ImagePart>& images);

// Hooks the UI uses to render the agent as it runs.
struct UiCallbacks {
  std::function<void(const std::string&)> text;  // streamed assistant text chunk
  std::function<void(const std::string&, const std::string&, const std::string&)>
      tool_call;  // (tool name, arg json, result)
  std::function<void(const std::string&)> system;  // one-line notice
  std::function<bool(const std::string&)> confirm;  // show plan, ask y/n; true = proceed
  std::function<void()> model_start;  // a round of model generation begins
  std::function<void(const std::string&)> reasoning;  // streamed reasoning chunk
  std::function<void()> round_end;  // the model finished a round (answer or tool calls)
  std::function<void(long, long, long)> usage;  // (in, out, total) for the round
  std::function<void(const std::string&)> status;  // set/clear the transient status message ("" = clear)
  std::function<void(const std::vector<ImagePart>&)>
      images;  // one or more images the model will now see (for display)
};

class AgentLoop {
public:
  AgentLoop(Config cfg, ToolRegistry* reg, std::string system_prompt, bool start_in_plan,
            UiCallbacks callbacks);

  // Run the loop for one user input until the model produces a final (no-tool) answer
  // or the max-turn cap is hit. Returns false on a model/transport error.
  // `images` (optional) carries images the user attached to this turn; they are
  // sent to the model alongside the text.
  bool run_turn(const std::string& user_input,
                const std::vector<ImagePart>* images = nullptr);

  void clear_context();
  // Request cooperative cancellation of the running (or next) turn. Safe to call
  // from another thread; the agent stops at the next checkpoint and run_turn
  // returns false.
  void stop();
  bool in_plan_mode() const { return plan_mode_; }
  // Plan-mode lifecycle (used by the model tools and the /plan command).
  // Idempotent; injects a system message so the model sees the state change.
  void enter_plan_mode();
  void exit_plan_mode();
  // The content of the most recent assistant message (empty if none). Used by the
  // `task` subagent to return its final answer.
  std::string last_assistant_text() const;

private:
  nlohmann::json tools_for_model() const;
  std::string dispatch_tool(const ToolCall& tc);
  std::string do_exit_plan_mode(const ToolCall& tc);
  // The single leading system message (base prompt + plan-mode rules when active).
  std::string current_system_message() const;
  void sync_system_message();
  // If the last round's prompt exceeded the compact threshold, summarize the older
  // messages into a single system note, keeping the system message and a safe tail.
  void maybe_compact();

  Config cfg_;
  ToolRegistry* reg_;
  std::string base_system_;
  nlohmann::json messages_;  // OpenAI message array
  UiCallbacks ui_;
  bool plan_mode_;
  long last_prompt_tokens_ = 0;  // most recent round's prompt_tokens (context size sent)
};

}  // namespace dog
