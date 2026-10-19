#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <termios.h>
#include "agent/loop.hpp"
#include "config/config.hpp"

namespace dog {

// Clipboard read result for the Ctrl-V handler: an image (is_image) or text.
struct ClipData {
  bool is_image = false;
  std::string mime;
  std::string data;  // image bytes when is_image
  std::string text;  // text when !is_image
};

// Terminal UI for the dog. In interactive mode it takes over the screen
// (alternate screen buffer, raw input) and keeps a scrolling transcript on top
// with a live status line and an editable input box pinned to the bottom row.
// In non-interactive mode (one-shot) it streams to stdout instead.
class Ui {
public:
  Ui(const Config& cfg, bool interactive);
  ~Ui();

  // --- Agent-facing hooks (fed by UiCallbacks). ---
  void on_text(const std::string& chunk);
  void on_reasoning(const std::string& chunk);
  void on_model_start();
  void on_round_end();
  void on_tool_start(const std::string& name, const std::string& args);
  void on_tool_call(const std::string& name, const std::string& args, const std::string& result);
  void on_system(const std::string& msg);
  void on_usage(long in, long out, long total);
  void on_status(const std::string& msg);  // set (or clear) the transient status line
  void on_images(const std::vector<ImagePart>& images);  // images the model will now see
  bool confirm(const std::string& plan);
  // A multiple-choice question the agent asks the user, answered by a picker that
  // takes the input box's place. `opts` are the suggested answers; the user may
  // also type their own. Returns the chosen label (or the typed text). When more
  // than one question is passed, all are answered in one card (tabs + Submit).
  struct Question {
    std::string header;  // short tab label ("Scope"); "" = "Q1", "Q2", ...
    std::string question;
    struct Choice {
      std::string label;
      std::string desc;  // shown under the label
    };
    std::vector<Choice> opts;
    bool multi = false;  // allow several answers
  };
  // Blocking: waits (on ask_cv_) until the user answers or the turn is
  // interrupted. `qs` are the questions; returns one answer each, or an empty
  // vector if interrupted. Non-interactive runs return an empty vector.
  std::vector<std::string> ask(const std::vector<Question>& qs);
  // --- Interactive-only. ---
  void user_message(const std::string& text);  // record a submitted user line
  // Drains (and clears) any images attached to the current line via Ctrl-V.
  std::vector<ImagePart> pop_pending_images();
  std::string read_input();                    // editable line; "" on EOF
  bool exit_requested() const { return exit_requested_; }
  bool alt() const { return alt_; }  // true when the alternate screen is active
  void clear_transcript();
  void on_turn_end();  // turn is over: stop the spinner

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
  bool history_nav(int dir);  // dir: -1 = up (older), +1 = down (newer)
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
  // Picker / ask_user.
  void pick_lines(std::vector<std::string>& out, int C);  // also sets pick_caret_line_/col_
  bool picker_key(int c);        // picker keys (Enter, digits, Space, Ctrl-C); true = consumed
  bool ask_key(int c);           // keys while a question is open; true = consumed
  void ask_show(size_t idx);     // save the current tab's state, show tab idx
  void ask_confirm();            // Enter on a question: record the answer, advance
  void pick_finish(int r);       // record the result and wake ask()
  void hint(const std::string& msg);
  size_t pick_rows() const;      // selectable rows under the cursor
  bool on_own_row() const;
  bool on_submit_row() const;
  bool submit_tab() const;
  ClipData read_clipboard();          // shell out to wl-paste; image or text
  void attach_image(const ClipData&); // record an attached image (re-render)
  std::string attach_line() const;    // one-liner for attached images ("" if none)

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
  bool working_ = false;         // a turn is in progress (model gen or tool work)
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
  std::vector<ImagePart> pending_images_;  // images attached via Ctrl-V this line
  std::string kill_ring_;  // last text killed (Ctrl-K / M-d), yanked by Ctrl-Y
  // In-memory prompt history (up/down arrows cycle through these, bash-style).
  std::vector<std::string> history_;
  int hist_idx_ = -1;  // -1 = free editing; else index into history_
  std::string suspended_input_;  // buffer before navigation began (restored on Down past newest)

  // Picker / ask_user state. While `picker_active_` the picker takes the input
  // box's place and draws its own caret; `pick_*` hold the question on screen and
  // `ask_*` hold the per-question state when several questions are shown as tabs.
  struct AskState {
    size_t sel = 0;
    std::vector<bool> marked;
    std::string typed;   // the "own answer" row's text while another tab is shown
    std::string answer;  // "" = not answered yet
  };
  bool picker_active_ = false;
  bool asking_ = false;
  std::vector<Question> ask_qs_;
  std::vector<AskState> ask_st_;
  size_t ask_idx_ = 0;  // == ask_qs_.size(): the Submit tab (several questions)
  std::string pick_title_;
  std::vector<Question::Choice> pick_opts_;
  bool pick_multi_ = false;
  size_t pick_sel_ = 0;
  std::vector<bool> pick_marked_;
  int pick_result_ = -2;  // -2 = pending, -1 = cancel, >= 0 = done
  std::string hint_;
  std::chrono::steady_clock::time_point hint_until_;
  std::condition_variable ask_cv_;
  int pick_caret_line_ = -1;  // line index (within picker lines) of the typed row; -1 = none
  int pick_caret_col_ = 0;    // display column of the typed caret

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
