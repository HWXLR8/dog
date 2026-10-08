#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <termios.h>
#include "agent/loop.hpp"
#include "config/config.hpp"

namespace dog {

// Terminal UI for the dog. In interactive mode it takes over the screen
// (alternate screen buffer, raw input) and keeps a scrolling transcript on top
// with a live status line and an editable input box pinned to the bottom row.
// In non-interactive mode (one-shot) it streams to stdout instead.
class Tui {
public:
  Tui(const Config& cfg, bool interactive);
  ~Tui();

  // --- Agent-facing hooks (fed by UiCallbacks). ---
  void on_text(const std::string& chunk);
  void on_reasoning(const std::string& chunk);
  void on_model_start();
  void on_round_end();
  void on_tool_call(const std::string& name, const std::string& args, const std::string& result);
  void on_system(const std::string& msg);
  void on_usage(long in, long out, long total);
  void on_status(const std::string& msg);  // set (or clear) the transient status line
  void on_images(const std::vector<ImagePart>& images);  // images the model will now see
  bool confirm(const std::string& plan);

  // --- Interactive-only. ---
  void user_message(const std::string& text);  // record a submitted user line
  std::string read_input();                    // editable line; "" on EOF
  bool exit_requested() const { return exit_requested_; }
  bool alt() const { return alt_; }  // true when the alternate screen is active
  void clear_transcript();

  // Non-blocking input poll for use while agent runs in background.
  enum class PollResult { kNone, kRedraw, kCancel };
  PollResult poll_input();

private:
  void redraw();
  void start_screen();
  void end_screen();
  void set_raw(bool on);
  bool read_one_byte(int* c);
  bool stdin_ready(int ms);
  PollResult handle_poll_byte(int c);
  bool apply_edit_key(int c);
  bool apply_esc_bytes();
  std::vector<std::string> content_lines() const;
  std::string status_string() const;
  std::string token_line_str() const;
  int term_rows() const;
  int term_cols() const;
  std::vector<std::string> render_item(int kind, const std::string& raw, int width) const;
  void one_shot_thinking_line();
  void one_shot_thinking_clear();
  void consume_paste(std::string pre);  // drain a bracketed paste into input_ literally
  void spinner_loop();  // timer-driven redraws so the status line animates

  struct Item {
    int kind = 0;  // 0 user, 1 assistant, 2 tool, 3 system
    std::string raw;
    std::vector<std::string> lines;
  };

  Config cfg_;
  bool interactive_ = false;
  bool alt_ = false;
  bool raw_active_ = false;
  std::vector<Item> items_;
  std::string header_raw_;

  // Live assistant message being streamed.
  std::string live_;
  // Cached render of live_ (markdown) so frequent redraws (spinner) are cheap:
  // markdown is re-parsed only when the streamed text (or width) changes.
  mutable std::vector<std::string> live_cached_;
  mutable std::string live_cache_key_;  // = live_ + " " + term_cols()

  // Live reasoning being streamed (shown in transcript in real-time).
  bool reasoning_active_ = false;

  // Status line state.
  bool thinking_ = false;
  bool answer_started_ = false;  // set once content starts streaming
  std::string reasoning_;  // accumulated reasoning (newlines collapsed)
  std::chrono::steady_clock::time_point think_start_;
  long session_in_ = 0, session_out_ = 0;
  long turn_in_ = 0, turn_out_ = 0;  // sum of all model rounds since the last user prompt
  long live_out_est_ = 0;            // in-progress round's streamed output estimate (resets each round)
  long ctx_used_ = 0;                // tokens in the most recent request (last round's prompt_tokens)
  std::string status_override_;

  // Input box.
  std::string input_;
  size_t cursor_ = 0;
  bool exit_requested_ = false;
  std::string kill_ring_;  // last text killed (Ctrl-K / M-d), yanked by Ctrl-Y

  // Scroll. 0 = at bottom (auto-follow). Positive = scrolled up N lines.
  int scroll_offset_ = 0;
  bool show_reasoning_ = false;

  // One-shot (non-interactive) thinking line.
  bool one_shot_line_ = false;

  // Redraw cache.
  std::string last_frame_;
  int last_rows_ = -1;
  int last_cols_ = -1;
  long last_tr_size_ = 0;

  // Serializes redraw + state mutation (main thread) against the spinner thread.
  std::mutex draw_mu_;
  std::thread spin_;
  std::atomic<bool> spin_run_{false};
};

}  // namespace dog
