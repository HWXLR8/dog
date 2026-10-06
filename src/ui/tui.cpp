#include "ui/tui.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include "ui/markdown.hpp"
#include "util.hpp"

namespace dog {

namespace {

const char* kSpinner[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

bool is_tty_out() { return ::isatty(STDOUT_FILENO) != 0; }

// Byte offset of the start of the rune that ends just before pos (i.e. whose last
// byte is s[pos-1]). Walks back over the rune's continuation bytes to its lead byte.
size_t rune_before(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  size_t j = pos - 1;
  while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) j--;
  return j;
}
// End byte offset of the rune starting at pos.
size_t rune_after(const std::string& s, size_t pos) {
  if (pos >= s.size()) return s.size();
  unsigned char b = static_cast<unsigned char>(s[pos]);
  int len = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
  return pos + len;
}
// True if byte b is part of a "word" (alphanumeric, underscore, or a non-ASCII
// rune byte). Spaces/punctuation/newlines are word boundaries.
bool is_word_char(unsigned char b) {
  if (b >= 0x80) return true;
  return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
         (b >= '0' && b <= '9') || b == '_';
}
// Byte offset at the end of the word beginning at/after pos: skip leading
// non-word chars, then run over the word itself (emacs M-f / M-d target).
size_t word_end(const std::string& s, size_t pos) {
  size_t p = pos;
  while (p < s.size() && !is_word_char((unsigned char)s[p])) p = rune_after(s, p);
  while (p < s.size() && is_word_char((unsigned char)s[p])) p = rune_after(s, p);
  return p;
}
// Byte offset at the start of the word immediately before pos: skip trailing
// non-word chars, then back off over the word itself (emacs M-b target).
size_t word_start(const std::string& s, size_t pos) {
  size_t p = pos;
  while (p > 0) {
    size_t q = rune_before(s, p);
    if (is_word_char((unsigned char)s[q])) break;
    p = q;
  }
  while (p > 0) {
    size_t q = rune_before(s, p);
    if (!is_word_char((unsigned char)s[q])) break;
    p = q;
  }
  return p;
}

// Word-wrap plain text (no ANSI) to `width` columns. Long tokens are hard-broken.
std::vector<std::string> wrap_plain(const std::string& text, int width) {
  if (width <= 0) width = 80;
  std::vector<std::string> out;
  std::string cur;
  int curw = 0;
  auto flush = [&]() {
    out.push_back(cur);
    cur.clear();
    curw = 0;
  };
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      flush();
      i++;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\n') j++;
    std::string word = text.substr(i, j - i);
    int ww = (int)word.size();
    auto place_long = [&]() {
      size_t pos = 0;
      while (pos < word.size()) {
        size_t take = std::min((size_t)width, word.size() - pos);
        out.push_back(word.substr(pos, take));
        pos += take;
      }
      curw = 0;
    };
    if (curw == 0) {
      if (ww <= width) {
        cur = word;
        curw = ww;
      } else
        place_long();
    } else if (curw + 1 + ww <= width) {
      cur += " " + word;
      curw += 1 + ww;
    } else {
      flush();
      if (ww <= width) {
        cur = word;
        curw = ww;
      } else
        place_long();
    }
    while (j < text.size() && text[j] == ' ') j++;
    i = j;
  }
  if (!cur.empty() || out.empty()) out.push_back(cur);
  return out;
}

std::string token_line(bool ansi, long in, long out, long s_in, long s_out,
                       long ctx_used, long ctx_window) {
  auto k = [](long n) {
    if (n < 1000) return render::fmt_int(n);
    long r = (n + 500) / 1000;  // round to the nearest 1K
    return std::to_string(r) + "K";
  };
  std::string num = "\xe2\x86\x91 " + k(in) + "  \xe2\x86\x93 " + k(out);
  std::string sess = "\xe2\x86\x91 " + k(s_in) + "  \xe2\x86\x93 " + k(s_out);
  std::string ctx;
  if (ctx_window > 0 && ctx_used > 0) {
    long pct = (long)((double)ctx_used / (double)ctx_window * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ctx = "\x1b[38;5;245mctx " + k(ctx_used) + "/" + k(ctx_window) + " " +
          std::to_string(pct) + "%\x1b[0m";
  }
  if (ctx.empty()) {
    if (!ansi) return "  " + num + "   \xc2\xb7   " + sess;
    return "\x1b[2m  " + num + "   \xc2\xb7   " + sess + "\x1b[0m";
  }
  if (!ansi) return "  " + num + "   \xc2\xb7   " + sess + "   \xc2\xb7   " + render::strip_ansi(ctx);
  return "\x1b[2m  " + num + "   \xc2\xb7   " + sess + "   \xc2\xb7   " + ctx + "\x1b[0m";
}

std::string spinner_thinking(const std::string& reasoning, long ms, int cols) {
  size_t f = (((ms / 80) % 10) + 10) % 10;
  int budget = cols - 14;
  if (budget < 8) budget = 8;
  std::string t = render::utf8_tail(dog::util::trim(reasoning), budget);
  std::string s = std::string(kSpinner[f]);
  if (!t.empty()) s += " \x1b[2m" + t + "\x1b[0m";
  return s;
}

}  // namespace

Tui::Tui(const Config& cfg, bool interactive) : cfg_(cfg), interactive_(interactive) {
  if (interactive_) {
    start_screen();
    header_raw_ = "dog   " + cfg_.model + "   " + cfg_.base_url;
    int C = term_cols();
    Item h;
    h.kind = 3;
    h.raw = header_raw_;
    h.lines = render_item(3, header_raw_, C);
    items_.push_back(std::move(h));
    redraw();
    // Start the timer-driven spinner only after initial setup so it never races
    // the constructor's first redraw.
    spin_run_ = true;
    spin_ = std::thread(&Tui::spinner_loop, this);
  }
}

Tui::~Tui() { end_screen(); }

void Tui::set_raw(bool on) {
  static termios saved;
  static bool has_saved = false;
  if (on) {
    if (raw_active_) return;
    if (tcgetattr(STDIN_FILENO, &saved) == 0) has_saved = true;
    termios t = saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~ICRNL;  // keep physical Enter as raw CR so it is distinct from pasted LF newlines
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    raw_active_ = true;
  } else if (raw_active_) {
    if (has_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    raw_active_ = false;
  }
}

void Tui::start_screen() {
  if (!is_tty_out() || !::isatty(STDIN_FILENO)) return;
  std::cout << "\x1b[?1049h"  // alt screen
            << "\x1b[2J"       // clear
            << "\x1b[H"        // home
            << "\x1b[?25l"     // hide cursor
            << "\x1b[?1006h"   // SGR mouse mode
            << "\x1b[?2004h";  // bracketed paste mode
  std::cout.flush();
  set_raw(true);
  alt_ = true;
}

void Tui::end_screen() {
  if (!alt_) return;
  spin_run_ = false;
  if (spin_.joinable()) spin_.join();
  set_raw(false);
  std::cout << "\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1049l\n";
  std::cout.flush();
  alt_ = false;
}

int Tui::term_rows() const {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
  return 24;
}
int Tui::term_cols() const {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
  return 80;
}

bool Tui::read_one_byte(int* c) {
  char ch = 0;
  ssize_t n = read(STDIN_FILENO, &ch, 1);
  if (n == 1) {
    *c = (unsigned char)ch;
    return true;
  }
  return false;
}

std::vector<std::string> Tui::render_item(int kind, const std::string& raw, int width) const {
  if (width <= 0) width = 80;
  if (kind == 1) {
    auto lines = render::markdown(raw, width);
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    return lines;
  }

  if (kind == 0) {  // user
    // The "> " / "  " prefix is 2 columns; wrap content to the remaining width so
    // the line never exceeds the terminal width (else it hard-wraps mid-word).
    auto w = wrap_plain(raw, width - 2);
    std::vector<std::string> lines;
    for (size_t i = 0; i < w.size(); i++)
      lines.push_back(i == 0 ? "\x1b[1m\x1b[38;5;39m>\x1b[0m " + w[i] : "  " + w[i]);
    return lines;
  }

  if (kind == 2) {  // tool
    size_t tab = raw.find('\t');
    std::string name = tab == std::string::npos ? raw : raw.substr(0, tab);
    std::string snippet = tab == std::string::npos ? "" : raw.substr(tab + 1);
    int budget = width - 2 - (int)name.size() - 2;
    if (budget < 4) budget = 4;
    auto w = wrap_plain(snippet, budget);
    std::vector<std::string> lines;
    std::string marker = "\x1b[2m\x1b[38;5;245m\x1b[2m·\x1b[0m ";
    if (w.empty()) {
      lines.push_back(marker + "\x1b[1m\x1b[38;5;117m" + name + "\x1b[0m");
    } else {
      lines.push_back(marker + "\x1b[1m\x1b[38;5;117m" + name + "\x1b[0m \x1b[2m" + w[0] +
                       "\x1b[0m");
      for (size_t i = 1; i < w.size(); i++)
        lines.push_back("  \x1b[2m" + w[i] + "\x1b[0m");
    }
    return lines;
  }

  if (kind == 4) {  // thinking (collapsible)
    std::vector<std::string> lines;
    if (!show_reasoning_) {
      lines.push_back("\x1b[2m\x1b[38;5;245m\xe2\x97\x88 thinking \x1b[0m");
      return lines;
    }
    lines.push_back("\x1b[2m\x1b[38;5;245m\xe2\x97\x88 thinking\x1b[0m");
    auto w = wrap_plain(raw, width - 2);
    for (auto& l : w) lines.push_back("  \x1b[2m" + l + "\x1b[0m");
    return lines;
  }

  if (kind == 5) {  // file diff
    size_t t1 = raw.find('\t');
    std::string tool_name = raw.substr(0, t1);
    // For edit_file, on_tool_call appends the line number: raw = "name\targs\t<line>".
    // args must be cut at the 2nd tab, otherwise the trailing "\t<line>" corrupts the
    // JSON parse below and the whole diff is dropped.
    std::string args_json = raw.substr(t1 + 1);
    size_t t2 = args_json.find('\t');
    if (t2 != std::string::npos) args_json = args_json.substr(0, t2);
    std::vector<std::string> lines;
    try {
      auto j = nlohmann::json::parse(args_json);
      std::string path = j.value("path", j.value("file_path", "?"));
      lines.push_back("\x1b[1m\x1b[38;5;208m" + path + "\x1b[0m");
      const char kGreen = 2;  // 38;5;46
      const char kRed = 1;    // 38;5;196
      auto add_line = [&](int color, const std::string& prefix, const std::string& text, int ln) {
        std::string c = color == kGreen ? "\x1b[38;5;46m" : "\x1b[38;5;196m";
        std::string gutter;
        if (ln > 0) {
          std::string num = std::to_string(ln);
          if (num.size() < 3) num = std::string(3 - num.size(), ' ') + num;
          gutter = "\x1b[2m" + num + "\x1b[0m ";
        }
        lines.push_back("  " + gutter + c + prefix + " " + text + "\x1b[0m");
      };
      if (tool_name == "write_file") {
        std::string content = j.value("content", "");
        auto c_lines = dog::util::split_lines(content);
        size_t max_show = 30;
        for (size_t i = 0; i < c_lines.size() && i < max_show; i++)
          add_line(kGreen, "+", c_lines[i], (int)i + 1);
        if (c_lines.size() > max_show) {
          long remaining = (long)c_lines.size() - (long)max_show;
          lines.push_back("  \x1b[2m\x1b[38;5;245m... +" + std::to_string(remaining) + " more lines\x1b[0m");
        }
      } else if (tool_name == "edit_file") {
        std::string old_s = j.value("old_string", "");
        std::string new_s = j.value("new_string", "");
        auto old_lines = dog::util::split_lines(old_s);
        auto new_lines2 = dog::util::split_lines(new_s);
        // Extract line number from raw (third tab-separated field).
        int base_ln = 0;
        size_t tab2 = raw.find('\t', raw.find('\t') + 1);
        if (tab2 != std::string::npos) {
          std::string ln_str = raw.substr(tab2 + 1);
          base_ln = std::atoi(ln_str.c_str());
        }
        size_t max_show = 20;
        size_t shown = 0;
        for (size_t i = 0; i < old_lines.size(); i++) {
          if (shown >= max_show) break;
          add_line(kRed, "-", old_lines[i], base_ln + (int)i);
          shown++;
        }
        if (old_lines.size() > max_show) {
          long rem = (long)old_lines.size() - (long)std::min(old_lines.size(), max_show);
          lines.push_back("  \x1b[2m\x1b[38;5;245m... -" + std::to_string(rem) + " more\x1b[0m");
        }
        for (size_t i = 0; i < new_lines2.size(); i++) {
          if (shown >= max_show) break;
          add_line(kGreen, "+", new_lines2[i], base_ln + (int)i);
          shown++;
        }
        if (new_lines2.size() > max_show - std::min(old_lines.size(), max_show)) {
          long rem = (long)new_lines2.size() - (long)(max_show - std::min(old_lines.size(), max_show));
          if (rem > 0) lines.push_back("  \x1b[2m\x1b[38;5;245m... +" + std::to_string(rem) + " more\x1b[0m");
        }
      }
    } catch (...) {
      lines.push_back("  \x1b[2m" + tool_name + "\x1b[0m");
    }
    if (lines.size() == 1) lines.push_back("");
    return lines;
  }

  // system / header
  auto w = wrap_plain(raw, width);
  std::vector<std::string> lines;
  for (auto& l : w) lines.push_back("\x1b[2m\x1b[38;5;245m" + l + "\x1b[0m");
  if (lines.empty()) lines.push_back("");
  return lines;
}

std::vector<std::string> Tui::content_lines() const {
  int C = term_cols();
  std::vector<std::string> out;
  for (size_t i = 0; i < items_.size(); i++) {
    for (auto& l : items_[i].lines) out.push_back(l);
    if (i + 1 < items_.size() || reasoning_active_ || !live_.empty()) out.push_back("");
  }
  if (reasoning_active_ && !reasoning_.empty()) {
    for (auto& l : render_item(4, reasoning_, C)) out.push_back(l);
    out.push_back("");
  }
  if (!live_.empty()) {
    auto ml = render::markdown(live_, C);
    size_t first = 0;
    while (first < ml.size() && ml[first].empty()) first++;
    for (size_t i = first; i < ml.size(); i++) out.push_back(ml[i]);
  }
  return out;
}

std::string Tui::status_string() const {
  std::string left;
  if (!status_override_.empty()) {
    left = status_override_;  // explicit transient status (e.g. compaction) wins
  } else if (thinking_ && !answer_started_) {
    long el = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - think_start_)
                   .count();
    size_t f = (((el / 80) % 10) + 10) % 10;
    left = std::string(kSpinner[f]);
  }
  return left;
}

std::string Tui::token_line_str() const {
  // Per-turn numbers = everything since the last user prompt (all rounds), plus
  // the in-progress round's live output estimate (reasoning + streamed text).
  long in_show = turn_in_;
  long out_show = turn_out_ + live_out_est_;
  long s_out = session_out_ + live_out_est_;
  return token_line(true, in_show, out_show, session_in_, s_out, ctx_used_, cfg_.context_window);
}

void Tui::redraw() {
  if (!alt_) return;
  int R = term_rows();
  int C = term_cols();
  if (C != last_cols_) {  // re-render on resize
    last_cols_ = C;
    for (auto& it : items_) it.lines = render_item(it.kind, it.raw, C);
  }
  std::vector<std::string> tr = content_lines();
  // The status line (thinking spinner / plan prompt) becomes the last line of the
  // transcript so it is bottom-anchored like everything else. The row just above
  // the input box (R-5) is then always left blank, so there is exactly one blank
  // line between the content and the input box in every state.
  std::string status = status_string();
  if (!status.empty()) {
    if (!tr.empty() && !tr.back().empty()) tr.push_back("");
    tr.push_back(status);
  }
  // The input box expands to fit the number of lines in input_ (multi-line via
  // Shift+Enter / paste). It grows upward so the bottom rule + token line stay
  // pinned to the bottom of the screen.
  std::vector<std::string> in_lines;
  {
    size_t ls = 0;
    for (size_t b = 0; b < input_.size(); b++)
      if (input_[b] == '\n') { in_lines.push_back(input_.substr(ls, b - ls)); ls = b + 1; }
    in_lines.push_back(input_.substr(ls));
  }
  int nlines = (int)in_lines.size();
  // Map cursor_ (byte offset in input_) to the line it's on and the byte col within it.
  size_t cur_line = 0, cur_off = 0;
  {
    for (size_t b = 0; b < input_.size(); b++) {
      if (b == cursor_) break;
      if (input_[b] == '\n') { cur_line++; cur_off = 0; }
      else cur_off++;
    }
  }
  if (cur_line >= in_lines.size()) cur_line = in_lines.size() - 1;

  int top_rule_row = R - 3 - nlines;  // single line -> R-4 (unchanged from before)
  if (top_rule_row < 1) top_rule_row = 1;
  int tr_h = top_rule_row - 1;  // transcript rows 0..tr_h-1; row tr_h is the blank gap
  if (tr_h < 0) tr_h = 0;

  // Index of the transcript line drawn at row 0. It is negative when the
  // transcript is shorter than the area, which means blank top padding so the
  // transcript is bottom-anchored. `off` scrolls up within the transcript.
  long base = (long)tr.size() - (long)tr_h;
  long max_up = base > 0 ? base : 0;
  long off = scroll_offset_;
  if (off < 0) off = 0;
  if (off > max_up) off = max_up;
  scroll_offset_ = off;  // clamp stored value so PgUp at top is a no-op
  long first = base - off;

  // Render one input line; the active line carries the reverse-video block cursor.
  auto render_input_line = [&](const std::string& il, const std::string& prefix, bool active, size_t col) -> std::string {
    std::string out = prefix;
    if (!active) return out + il;
    size_t i = 0;
    while (i < il.size()) {
      size_t end = rune_after(il, i);
      if (i == col) out += "\x1b[7m" + il.substr(i, end - i) + "\x1b[27m";
      else out += il.substr(i, end - i);
      i = end;
    }
    if (col == il.size()) out += "\x1b[7m \x1b[27m";  // caret past the last char
    return out;
  };

  std::string tokens = token_line_str();

  std::string frame;
  frame.reserve((size_t)R * 48);
  frame += "\x1b[H";
  for (int i = 0; i < R; i++) {
    std::string line;
    if (i < tr_h) {
      long idx = first + i;
      if (idx >= 0 && idx < (long)tr.size())
        line = tr[idx];
      // else blank: top padding while the transcript is shorter than the area
    } else if (i == top_rule_row || i == R - 2) {
      // Full-width horizontal rules around the input box (above and below it), dimmed.
      line.clear();
      line.reserve((size_t)C * 3 + 8);
      line += "\x1b[2m";
      for (int c = 0; c < C; c++) line += "\xe2\x94\x80";
      line += "\x1b[0m";
    } else if (i == R - 1) {
      line = tokens;
    } else if (i > top_rule_row && i < R - 2) {
      int li = i - top_rule_row - 1;  // which input line this screen row holds
      if (li >= 0 && li < (int)in_lines.size())
        line = render_input_line(in_lines[li], li == 0 ? "> " : "  ", li == (int)cur_line, cur_off);
    }
    // i == tr_h (the gap) and any overflow input rows are left blank.
    frame += line;
    frame += "\x1b[K";
    if (i != R - 1) frame += "\n";
  }
  int hw_row = top_rule_row + 1 + (int)cur_line;
  int cursor_col = 2 + render::display_width(in_lines[cur_line].substr(0, cur_off));
  if (hw_row < 1) hw_row = 1;
  if (hw_row > R - 1) hw_row = R - 1;
  if (cursor_col > C) cursor_col = C;
  frame += "\x1b[" + std::to_string(hw_row) + ";" + std::to_string(cursor_col + 1) + "H";

  if (frame == last_frame_ && R == last_rows_) {
    std::cout.flush();
    return;
  }
  last_frame_ = frame;
  last_rows_ = R;
  std::cout << frame << std::flush;
}

// --- Agent-facing hooks ---

void Tui::on_model_start() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  thinking_ = true;
  answer_started_ = false;
  live_out_est_ = 0;
  reasoning_active_ = false;
  reasoning_.clear();
  status_override_.clear();  // a fresh model round supersedes any transient status
  think_start_ = std::chrono::steady_clock::now();
  if (interactive_)
    redraw();
  else
    one_shot_thinking_line();
}

void Tui::on_reasoning(const std::string& c) {
  if (c.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  reasoning_active_ = true;
  for (char ch : c) reasoning_ += (ch == '\n' || ch == '\r') ? ' ' : ch;
  live_out_est_ = (live_.size() + reasoning_.size()) / 4;
  if (interactive_)
    redraw();
  else
    one_shot_thinking_line();
}

void Tui::on_text(const std::string& c) {
  if (c.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  answer_started_ = true;
  live_ += c;
  live_out_est_ = (live_.size() + reasoning_.size()) / 4;
  if (interactive_) redraw();
  // one-shot: buffered, rendered on round_end to keep markdown clean
}

void Tui::on_round_end() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  thinking_ = false;
  if (reasoning_active_ && !reasoning_.empty()) {
    int C = term_cols();
    Item it;
    it.kind = 4;
    it.raw = reasoning_;
    it.lines = render_item(4, reasoning_, C);
    items_.push_back(std::move(it));
  }
  reasoning_active_ = false;
  reasoning_.clear();
  std::vector<std::string> new_lines;
  if (!live_.empty()) {
    std::string raw = std::move(live_);
    live_.clear();
    int C = term_cols();
    new_lines = render_item(1, raw, C);
    Item it;
    it.kind = 1;
    it.raw = raw;
    it.lines = new_lines;
    items_.push_back(std::move(it));
  }
  if (interactive_) {
    redraw();
  } else {
    one_shot_thinking_clear();
    bool ansi = is_tty_out();
    for (auto& l : new_lines) std::cout << (ansi ? l : render::strip_ansi(l)) << "\n";
    if (!new_lines.empty()) std::cout << "\n";
    std::cout.flush();
  }
}

void Tui::on_tool_call(const std::string& name, const std::string& args, const std::string& result) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  bool is_file_op = (name == "write_file" || name == "edit_file");
  int kind = is_file_op ? 5 : 2;
  std::string raw = name + "\t" + args;
  // For edit_file, embed the line number from the result so render can use it.
  if (name == "edit_file" && result.find("at line ") != std::string::npos) {
    size_t p = result.find("at line ") + 8;
    size_t e = result.find(' ', p);
    raw += "\t" + result.substr(p, e - p);
  }
  it.kind = kind;
  it.raw = raw;
  it.lines = render_item(kind, raw, C);
  items_.push_back(std::move(it));
  if (interactive_) {
    redraw();
  } else {
    std::string s = "  " + name;
    if (!result.empty()) s += "  " + result.substr(0, 60);
    std::cout << (is_tty_out() ? "\x1b[2m" + s + "\x1b[0m" : s) << "\n";
    std::cout.flush();
  }
}

void Tui::on_system(const std::string& msg) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  it.kind = 3;
  it.raw = msg;
  it.lines = render_item(3, msg, C);
  items_.push_back(std::move(it));
  if (interactive_) {
    redraw();
  } else {
    std::cout << (is_tty_out() ? "\x1b[2m" + msg + "\x1b[0m" : msg) << "\n";
    std::cout.flush();
  }
}

void Tui::on_usage(long in, long out, long total) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  (void)total;
  session_in_ += in;
  session_out_ += out;
  turn_in_ += in;
  turn_out_ += out;
  ctx_used_ = in;  // this round's prompt_tokens = size of the request just sent = context consumed
  live_out_est_ = 0;  // this round is now finalized into the turn totals
  if (interactive_) {
    redraw();
  } else {
    std::cout << token_line(is_tty_out(), turn_in_, turn_out_, session_in_, session_out_,
                            ctx_used_, cfg_.context_window)
              << "\n";
    std::cout.flush();
  }
}

void Tui::on_status(const std::string& msg) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  status_override_ = msg;
  if (interactive_) {
    redraw();
  } else if (!msg.empty()) {
    std::cout << (is_tty_out() ? "\x1b[2m" + msg + "\x1b[0m" : msg) << "\n";
    std::cout.flush();
  }
}

void Tui::on_images(const std::vector<ImagePart>& images) {
  if (images.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  it.kind = 3;
  it.raw = "[image] ";
  for (size_t i = 0; i < images.size(); i++) {
    std::string p = images[i].path.empty() ? images[i].mime : images[i].path;
    if (i) it.raw += ", ";
    it.raw += p + " (" + std::to_string(images[i].data.size()) + " bytes)";
  }
  it.lines = render_item(3, it.raw, C);
  items_.push_back(std::move(it));
  if (interactive_) {
    redraw();
  } else {
    std::cout << (is_tty_out() ? "\x1b[2m" + it.raw + "\x1b[0m" : it.raw) << "\n";
    std::cout.flush();
  }
}

bool Tui::confirm(const std::string& plan) {
  if (!interactive_) {
    std::cout << "(non-interactive: plan auto-approved)\n";
    return true;
  }
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  it.kind = 1;
  it.raw = plan;
  it.lines = render_item(1, plan, C);
  items_.push_back(std::move(it));
  std::string saved_input = input_;
  size_t saved_cursor = cursor_;
  input_.clear();
  cursor_ = 0;
  status_override_ = "\x1b[1m\x1b[38;5;220mapprove plan?  (y = yes, n = no)\x1b[0m";
  redraw();
  char ch = 'n';
  bool got = false;
  while (!got) {
    int c = 0;
    if (!read_one_byte(&c)) break;
    if (c == 'y' || c == 'Y') {
      ch = 'y';
      got = true;
    } else if (c == 'n' || c == 'N') {
      ch = 'n';
      got = true;
    } else if (c == '\r' || c == '\n') {
      ch = 'y';
      got = true;
    } else if (c == 0x03) {
      ch = 'n';
      got = true;
    }
  }
  bool ok = (ch == 'y');
  status_override_.clear();
  input_ = saved_input;
  cursor_ = saved_cursor;
  Item d;
  d.kind = 3;
  d.raw = ok ? "plan approved" : "plan rejected";
  d.lines = render_item(3, d.raw, C);
  items_.push_back(std::move(d));
  redraw();
  return ok;
}

// --- Interactive input ---

void Tui::user_message(const std::string& text) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  it.kind = 0;
  it.raw = text;
  it.lines = render_item(0, text, C);
  items_.push_back(std::move(it));
  turn_in_ = 0;
  turn_out_ = 0;
  live_out_est_ = 0;
  input_.clear();
  cursor_ = 0;
  redraw();
}

void Tui::clear_transcript() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  items_.clear();
  scroll_offset_ = 0;
  ctx_used_ = 0;
  int C = term_cols();
  Item h;
  h.kind = 3;
  h.raw = header_raw_;
  h.lines = render_item(3, header_raw_, C);
  items_.push_back(std::move(h));
  if (interactive_) redraw();
}

std::string Tui::read_input() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  for (;;) {
    int c = 0;
    if (!read_one_byte(&c)) {
      exit_requested_ = true;
      std::string s = input_;
      input_.clear();
      cursor_ = 0;
      redraw();
      return s;
    }
    if (c == '\r') {
      std::string s = input_;
      input_.clear();
      cursor_ = 0;
      redraw();
      return s;
    } else if (c == 0x7f || c == 0x08) {
      if (cursor_ > 0) {
        size_t from = rune_before(input_, cursor_);
        input_.erase(from, cursor_ - from);
        cursor_ = from;
      }
    } else if (c == 0x03) {  // Ctrl-C
      if (!input_.empty()) {
        input_.clear();
        cursor_ = 0;
      } else {
        exit_requested_ = true;
        std::string s = input_;
        input_.clear();
        redraw();
        return s;
      }
    } else if (c == 0x04) {  // Ctrl-D
      if (input_.empty()) {
        exit_requested_ = true;
        std::string s = input_;
        input_.clear();
        redraw();
        return s;
      }
      size_t to = rune_after(input_, cursor_);
      input_.erase(cursor_, to - cursor_);
    } else if (c == 0x15) {
      input_.clear();
      cursor_ = 0;  // Ctrl-U
    } else if (c == 0x01) {
      cursor_ = 0;  // Ctrl-A: beginning of line
    } else if (c == 0x02) {
      if (cursor_ > 0) cursor_ = rune_before(input_, cursor_);  // Ctrl-B: backward one char
    } else if (c == 0x05) {
      cursor_ = input_.size();  // Ctrl-E: end of line
    } else if (c == 0x06) {
      if (cursor_ < input_.size()) cursor_ = rune_after(input_, cursor_);  // Ctrl-F: forward
    } else if (c == 0x0b) {
      kill_ring_ = input_.substr(cursor_);  // Ctrl-K: kill to end of line
      input_.erase(cursor_);
    } else if (c == 0x19) {  // Ctrl-Y: yank kill ring (0x19 = 'Y' & 0x1f; 0x18 is Ctrl-X)
      input_.insert(cursor_, kill_ring_);
      cursor_ += kill_ring_.size();
    } else if (c == 0x0f) {
      show_reasoning_ = !show_reasoning_;  // Ctrl-O: toggle thinking visibility
      int C = term_cols();
      for (auto& it : items_)
        if (it.kind == 4) it.lines = render_item(4, it.raw, C);
    } else if (c == 0x1b) {
      // Read the full escape sequence in one bulk read
      unsigned char buf[32] = {0};
      int n = 0;
      {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (r > 0) n = (int)r;
      }
      int i = 0;
      if (n >= 5 && buf[0] == '[' && buf[1] == '2' && buf[2] == '0' && buf[3] == '0' && buf[4] == '~') {
        // Bracketed paste start (ESC[200~ ... ESC[201~): insert the whole blob literally
        // so pasted newlines never submit the prompt; only a physical Enter does.
        std::string pre((const char*)&buf[5], (size_t)(n - 5));
        consume_paste(std::move(pre));
        i = n;  // paste already drained; skip generic sequence processing below
      }
      // Meta (ESC) + a normal character: emacs-style M-key. The ESC itself was
      // already consumed by read_one_byte, so buf holds only the trailing bytes
      // (e.g. "f" for M-f, 1 byte).
      if (i == 0 && n >= 1) {
        unsigned char mc = (unsigned char)buf[0];
        if (mc < 0x20 && !(mc == '[' || mc == 'O')) {
          // ESC + control char is not a meta key we handle; ignore it.
          i = n;
        } else if (mc == 'f') {
          // M-f: forward one word
          if (cursor_ < input_.size()) cursor_ = word_end(input_, cursor_);
          i = n;
        } else if (mc == 'b') {
          // M-b: backward one word
          if (cursor_ > 0) cursor_ = word_start(input_, cursor_);
          i = n;
        } else if (mc == 'd') {
          // M-d: kill forward one word
          size_t e = word_end(input_, cursor_);
          if (e > cursor_) {
            kill_ring_ = input_.substr(cursor_, e - cursor_);
            input_.erase(cursor_, e - cursor_);
          }
          i = n;
        }
        // (other meta chars fall through to the generic sequence parser, which
        // skips unknown escape sequences.)
      }
      // First sequence (no leading ESC in buffer)
      if (i == 0 && n >= 2) {
        if (n == 9 && buf[0] == '[' && buf[1] == '2' && buf[2] == '7' &&
            buf[3] == ';' && buf[4] == '2' && buf[5] == ';' && buf[6] == '1' &&
            buf[7] == '3' && buf[8] == '~') {
          // Shift+Enter (xterm-style modified Enter, ESC[27;2;13~): insert a newline.
          input_.insert(cursor_, 1, '\n');
          cursor_++;
          i = n;  // consumed; skip generic handling below
        } else if (buf[0] == '[' && buf[1] == '<') {
          // SGR mouse: [<Btn;Col;RowM|m>
          i = 2;
          int btn = 0;
          while (i < n && buf[i] >= '0' && buf[i] <= '9') { btn = btn * 10 + (buf[i] - '0'); i++; }
          if (btn == 64) scroll_offset_++;
          else if (btn == 65) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
          // skip rest of sequence (;Col;RowM|m)
          while (i < n && buf[i] != 0x1b) i++;
        } else if (buf[0] == '[' && n >= 2) {
          char p = (char)buf[1]; i = 2;
          if (p == 'A') scroll_offset_++;
          else if (p == 'B') { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
          else if (p == 'D') { if (cursor_ > 0) cursor_ = rune_before(input_, cursor_); }
          else if (p == 'C') { if (cursor_ < input_.size()) cursor_ = rune_after(input_, cursor_); }
          else if (p == 'H') cursor_ = 0;
          else if (p == 'F') cursor_ = input_.size();
          else if (p == '5') scroll_offset_ += 10;
          else if (p == '6') { scroll_offset_ -= 10; if (scroll_offset_ < 0) scroll_offset_ = 0; }
          else if (p >= '0' && p <= '9') {
            int num = p - '0';
            while (i < n && buf[i] >= '0' && buf[i] <= '9') { num = num * 10 + (buf[i] - '0'); i++; }
            if (i < n && buf[i] == ';') {
              if (num == 64) scroll_offset_++;
              else if (num == 65) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
            }
          }
          // skip to next ESC or end
          while (i < n && buf[i] != 0x1b) i++;
        } else if (buf[0] == 'O' && n >= 2) {
          char p = (char)buf[1]; i = 2;
          if (p == 'D') { if (cursor_ > 0) cursor_ = rune_before(input_, cursor_); }
          else if (p == 'C') { if (cursor_ < input_.size()) cursor_ = rune_after(input_, cursor_); }
          else if (p == 'H') cursor_ = 0;
          else if (p == 'F') cursor_ = input_.size();
        }
      }
      // Remaining sequences (each starts with ESC)
      while (i < n) {
        if (buf[i] == 0x1b && i + 1 < n && buf[i+1] == '[') {
          i += 2;
          if (i < n && buf[i] == '<') {
            i++;
            int btn = 0;
            while (i < n && buf[i] >= '0' && buf[i] <= '9') { btn = btn * 10 + (buf[i] - '0'); i++; }
            if (btn == 64) scroll_offset_++;
            else if (btn == 65) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
            while (i < n && buf[i] != 0x1b) i++;
          } else if (i < n) {
            char p = (char)buf[i]; i++;
            if (p == 'A') scroll_offset_++;
            else if (p == 'B') { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
            else if (p == 'D') { if (cursor_ > 0) cursor_ = rune_before(input_, cursor_); }
            else if (p == 'C') { if (cursor_ < input_.size()) cursor_ = rune_after(input_, cursor_); }
            else if (p == 'H') cursor_ = 0;
            else if (p == 'F') cursor_ = input_.size();
            else if (p == '5') scroll_offset_ += 10;
            else if (p == '6') { scroll_offset_ -= 10; if (scroll_offset_ < 0) scroll_offset_ = 0; }
            else if (p >= '0' && p <= '9') {
              int num = p - '0';
              while (i < n && buf[i] >= '0' && buf[i] <= '9') { num = num * 10 + (buf[i] - '0'); i++; }
              if (i < n && buf[i] == ';') {
                if (num == 64) scroll_offset_++;
                else if (num == 65) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
              }
            }
            while (i < n && buf[i] != 0x1b) i++;
          }
        } else if (buf[i] == 0x1b && i + 1 < n && buf[i+1] == 'O') {
          i += 2;
          if (i < n) {
            char p = (char)buf[i]; i++;
            if (p == 'D') { if (cursor_ > 0) cursor_ = rune_before(input_, cursor_); }
            else if (p == 'C') { if (cursor_ < input_.size()) cursor_ = rune_after(input_, cursor_); }
            else if (p == 'H') cursor_ = 0;
            else if (p == 'F') cursor_ = input_.size();
          }
          while (i < n && buf[i] != 0x1b) i++;
        } else {
          i++;
        }
      }
    } else if (c == '\n') {  // pasted newline: insert as literal (only Enter submits)
      input_.insert(cursor_, 1, '\n');
      cursor_++;
    } else if (c >= 0x20) {
      input_.insert(cursor_, 1, (char)c);
      cursor_++;
    }
    redraw();
  }
}

void Tui::consume_paste(std::string pre) {
  input_.insert(cursor_, pre);
  cursor_ += pre.size();
  const std::string end_marker = "\x1b[201~";  // bracketed paste end
  const size_t m = end_marker.size();
  std::string tail;
  for (;;) {
    unsigned char ch;
    ssize_t r = read(STDIN_FILENO, &ch, 1);
    if (r != 1) break;  // EOF: paste without a clean end marker
    tail.push_back((char)ch);
    if (tail.size() > m) tail.erase(0, tail.size() - m);
    input_.insert(cursor_, 1, (char)ch);  // insert literally (newlines included)
    cursor_++;
    if (tail.size() == m && tail == end_marker) {
      input_.erase(input_.size() - m, m);  // strip the end marker we just inserted
      cursor_ -= m;
      return;
    }
  }
}

Tui::PollResult Tui::poll_input() {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  timeval tv{0, 0};
  if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
    return PollResult::kNone;

  int c = 0;
  if (!read_one_byte(&c))
    return PollResult::kNone;

  // Ctrl-C: request a clean interrupt of the running turn (the agent stops at
  // its next checkpoint and returns to the prompt).
  if (c == 0x03) {
    util::request_interrupt();
    return PollResult::kCancel;
  }

  // Line editing: the input box is a live line buffer, so keystrokes typed while
  // a turn is running are applied to input_/cursor_ (not discarded) and the box
  // is redrawn. This mirrors the editing keys handled in read_input(). The lock
  // serializes against the spinner/agent threads that also call redraw().
  {
    std::lock_guard<std::mutex> lk(draw_mu_);
    bool changed = false;
    if (c == 0x7f || c == 0x08) {  // Backspace / DEL
      if (cursor_ > 0) {
        size_t from = rune_before(input_, cursor_);
        input_.erase(from, cursor_ - from);
        cursor_ = from;
      }
      changed = true;
    } else if (c == 0x15) {  // Ctrl-U: kill to start of line
      input_.erase(0, cursor_);
      cursor_ = 0;
      changed = true;
    } else if (c == 0x01) {  // Ctrl-A: beginning of line
      if (cursor_ != 0) { cursor_ = 0; changed = true; }
    } else if (c == 0x05) {  // Ctrl-E: end of line
      if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; }
    } else if (c == 0x06) {  // Ctrl-F: move forward one rune
      if (cursor_ < input_.size()) { cursor_ = rune_after(input_, cursor_); changed = true; }
    } else if (c == 0x0b) {  // Ctrl-K: kill to end of line
      input_.erase(cursor_);
      changed = true;
    } else if (c == 0x0f) {  // Ctrl-O: toggle thinking visibility
      show_reasoning_ = !show_reasoning_;
      int C = term_cols();
      for (auto& it : items_)
        if (it.kind == 4) it.lines = render_item(4, it.raw, C);
      changed = true;
    } else if (c >= 0x20) {  // printable (incl. high UTF-8 bytes)
      input_.insert(cursor_, 1, (char)c);
      cursor_++;
      changed = true;
    }
    if (changed) {
      redraw();
      return PollResult::kRedraw;
    }
  }

  if (c == 0x1b) {
    unsigned char buf[32] = {0};
    int n = 0;
    ssize_t r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (r > 0) n = (int)r;

    std::lock_guard<std::mutex> lk(draw_mu_);
    bool changed = false;
    int i = 0;
    // First sequence: leading ESC already consumed, buf starts with "[..."
    if (n >= 2 && buf[0] == '[') {
      char p = (char)buf[1]; i = 2;
      if (n == 9 && p == '2' && buf[2] == '7' && buf[3] == ';' &&
          buf[4] == '2' && buf[5] == ';' && buf[6] == '1' && buf[7] == '3' &&
          buf[8] == '~') {
        // Shift+Enter (ESC[27;2;13~): insert a newline.
        input_.insert(cursor_, 1, '\n');
        cursor_++;
        changed = true;
        i = n;
      } else if (p == 'A') { scroll_offset_++; changed = true; }
      else if (p == 'B') { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; }
      else if (p == 'D') { if (cursor_ > 0) { cursor_ = rune_before(input_, cursor_); changed = true; } }
      else if (p == 'C') { if (cursor_ < input_.size()) { cursor_ = rune_after(input_, cursor_); changed = true; } }
      else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
      else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
      else if (p == '5') { scroll_offset_ += 10; changed = true; }
      else if (p == '6') { scroll_offset_ -= 10; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; }
      while (i < n && buf[i] != 0x1b) i++;
    }
    // Remaining sequences (each starts with ESC)
    while (i < n) {
      if (buf[i] == 0x1b && i + 1 < n && buf[i+1] == '[') {
        i += 2;
        if (i < n && buf[i] == '<') {
          i++;
          int btn = 0;
          while (i < n && buf[i] >= '0' && buf[i] <= '9') { btn = btn * 10 + (buf[i] - '0'); i++; }
          if (btn == 64) { scroll_offset_++; changed = true; }
          else if (btn == 65) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; }
          while (i < n && buf[i] != 0x1b) i++;
        } else if (i < n) {
          char p = (char)buf[i]; i++;
          if (p == 'A') { scroll_offset_++; changed = true; }
          else if (p == 'B') { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; }
          else if (p == 'D') { if (cursor_ > 0) { cursor_ = rune_before(input_, cursor_); changed = true; } }
          else if (p == 'C') { if (cursor_ < input_.size()) { cursor_ = rune_after(input_, cursor_); changed = true; } }
          else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
          else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
          else if (p == '5') { scroll_offset_ += 10; changed = true; }
          else if (p == '6') { scroll_offset_ -= 10; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; }
          while (i < n && buf[i] != 0x1b) i++;
        }
      } else {
        i++;
      }
    }
    if (changed) {
      redraw();
      return PollResult::kRedraw;
    }
  }
  return PollResult::kNone;
}

void Tui::spinner_loop() {
  while (spin_run_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::lock_guard<std::mutex> lk(draw_mu_);
    redraw();
  }
}

void Tui::one_shot_thinking_line() {
  if (!is_tty_out()) return;
  int C = term_cols();
  long el = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - think_start_)
                  .count();
  std::cout << "\r\x1b[K" << spinner_thinking(reasoning_, el, C) << std::flush;
  one_shot_line_ = true;
}

void Tui::one_shot_thinking_clear() {
  if (one_shot_line_) {
    std::cout << "\r\x1b[K\n";
    one_shot_line_ = false;
    std::cout.flush();
  }
}

}  // namespace dog
