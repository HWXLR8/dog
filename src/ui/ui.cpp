#include "ui/ui.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include "json/json.hpp"
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include "ui/markdown.hpp"
#include "util.hpp"

namespace dog {

namespace {

const char* kSpinner[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

// Human-readable byte count: "47031" -> "46.0 KB".
std::string fmt_bytes(size_t n) {
  if (n < 1000) return std::to_string(n) + " B";
  if (n < 1000000)
    return std::to_string((n * 10 + 50000) / 100000) +
           "." + std::to_string((n * 10 % 100000) / 10000) + " KB";
  return std::to_string((n * 10 + 500000) / 1000000) +
         "." + std::to_string((n * 10 % 1000000) / 100000) + " MB";
}

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

// Column-hard-wrap that preserves EVERY character (runs and trailing spaces) and
// breaks on newlines. Each output row is <= width display columns. Used for the
// editable input box (wrap_plain normalizes whitespace and would desync the caret).
std::vector<std::string> wrap_input(const std::string& text, int width) {
  if (width <= 0) width = 80;
  std::vector<std::string> out;
  std::string cur;
  int curw = 0;
  auto flush = [&]() { out.push_back(cur); cur.clear(); curw = 0; };
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') { flush(); i++; continue; }  // newline = hard break
    size_t e = rune_after(text, i);
    int w = render::display_width(text.substr(i, e - i));
    if (curw + w > width && !cur.empty()) flush();     // width overflow = soft break
    cur.append(text, i, e - i);
    curw += w;
    i = e;
  }
  flush();  // final row (empty for empty input or a trailing newline)
  return out;
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

Ui::Ui(const Config& cfg, bool interactive) : cfg_(cfg), interactive_(interactive) {
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
    spin_ = std::thread(&Ui::spinner_loop, this);
  }
}

Ui::~Ui() { end_screen(); }

// Shell out to wl-paste (Wayland) to read the clipboard. Returns an image part
// (from the first image/* target) or, failing that, the text clipboard. The
// popen + Wayland IPC round-trip is run by the caller OUTSIDE draw_mu_ so the
// spinner thread is not blocked on it.
ClipData Ui::read_clipboard() {
  ClipData out;
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  if (!wayland || !*wayland) return out;  // not Wayland: nothing to read
  // 1. Discover targets: one MIME type per line.
  std::string list;
  if (FILE* f = popen("wl-paste --list-types 2>/dev/null", "r")) {
    char buf[256];
    while (size_t r = std::fread(buf, 1, sizeof buf, f))
      list.append(buf, r);
    pclose(f);
  }
  // 2. Prefer image/png, else the first image/* type.
  std::string best;
  for (auto& line : util::split_lines(list)) {
    std::string t = util::trim(line);
    if (t.compare(0, 6, "image/") != 0) continue;
    if (t == "image/png") { best = t; break; }
    if (best.empty()) best = t;
  }
  if (!best.empty()) {
    std::string cmd = "wl-paste -t '" + best + "' --no-newline 2>/dev/null";
    if (FILE* f = popen(cmd.c_str(), "r")) {
      char buf[8192];
      while (size_t r = std::fread(buf, 1, sizeof buf, f))
        out.data.append(buf, r);
      pclose(f);
    }
    if (!out.data.empty()) {
      out.is_image = true;
      out.mime = best;
      return out;
    }
  }
  // 3. No image: fall back to the text clipboard.
  if (FILE* f = popen("wl-paste --no-newline 2>/dev/null", "r")) {
    char buf[8192];
    while (size_t r = std::fread(buf, 1, sizeof buf, f))
      out.text.append(buf, r);
    pclose(f);
  }
  return out;
}

void Ui::attach_image(const ClipData& c) {
  ImagePart im;
  im.mime = c.mime.empty() ? "image/png" : c.mime;
  im.data = c.data;
  im.path = "clipboard";
  pending_images_.push_back(std::move(im));
  redraw();
}

std::string Ui::attach_line() const {
  if (pending_images_.empty()) return "";
  std::string s;
  for (size_t i = 0; i < pending_images_.size(); i++) {
    if (i) s += "  ";
    s += (pending_images_[i].mime.empty() ? "image" : pending_images_[i].mime) +
         " \xc2\xb7 " + fmt_bytes(pending_images_[i].data.size());
  }
  return "  \xF0\x9F\x93\x8E " + s;  // 2-space indent + paperclip (ANSI applied at draw)
}

std::vector<ImagePart> Ui::pop_pending_images() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  auto out = std::move(pending_images_);
  pending_images_.clear();
  redraw();
  return out;
}

void Ui::set_raw(bool on) {
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

void Ui::start_screen() {
  if (!is_tty_out() || !::isatty(STDIN_FILENO)) return;
  std::cout << "\x1b[?1049h"  // alt screen
            << "\x1b[2J"       // clear
            << "\x1b[H"        // home
            << "\x1b[?1006h"   // SGR mouse mode
            << "\x1b[?2004h";  // bracketed paste mode
  std::cout.flush();
  set_raw(true);
  alt_ = true;
}

void Ui::end_screen() {
  if (!alt_) return;
  spin_run_ = false;
  if (spin_.joinable()) spin_.join();
  set_raw(false);
  std::cout << "\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1049l\n";
  std::cout.flush();
  alt_ = false;
}

int Ui::term_rows() const {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
  return 24;
}
int Ui::term_cols() const {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
  return 80;
}

bool Ui::read_one_byte(int* c) {
  char ch = 0;
  ssize_t n = read(STDIN_FILENO, &ch, 1);
  if (n == 1) {
    *c = (unsigned char)ch;
    return true;
  }
  return false;
}

std::vector<std::string> Ui::render_item(int kind, const std::string& raw, int width) const {
  if (width <= 0) width = 80;
  if (kind == 1) {
    // Leave a 1-column guard: a line rendered at exactly `width` (== terminal
    // cols) parks the cursor at the right margin, and the frame's trailing
    // \x1b[K then erases that last cell, dropping the line's final character.
    auto lines = render::markdown(raw, width - 1);
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    return lines;
  }

  if (kind == 0) {  // user
    // The "> " / "  " prefix is 2 columns; wrap content to the remaining width so
    // the line never exceeds the terminal width (else it hard-wraps mid-word).
    // width-3 (not width-2) keeps a 1-column guard: a line rendered at exactly
    // `width` parks the cursor at the right margin and the frame's trailing
    // \x1b[K erases its last cell, dropping the line's final character.
    auto w = wrap_plain(raw, width - 3);
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
      auto j = dog::json::parse(args_json);
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

  if (kind == 6) {  // clipboard image attach: raw = pre-formatted "mime · size"
    auto w = wrap_plain(raw, width - 1);
    std::vector<std::string> lines;
    for (auto& l : w) lines.push_back("  \xF0\x9F\x93\x8E \x1b[38;5;208m" + l + "\x1b[0m");
    if (lines.empty()) lines.push_back("");
    return lines;
  }

  if (kind == 7) {  // answered question: "· question → answer"
    size_t tab = raw.find('\t');
    std::string q = raw.substr(0, tab);
    std::string a = tab == std::string::npos ? "" : raw.substr(tab + 1);
    if (a.empty()) a = "(no answer)";
    const std::string dot = "  \x1b[2m\xc2\xb7\x1b[0m ";
    const std::string arrow = "\x1b[2m\xe2\x86\x92\x1b[0m ";
    const std::string blue = "\x1b[38;5;153m";
    std::vector<std::string> lines;
    if (render::display_width(q) + render::display_width(a) + 7 <= width) {
      lines.push_back(dot + q + " " + arrow + blue + a + "\x1b[0m");
      return lines;
    }
    auto qw = wrap_plain(q, width - 4);
    for (size_t i = 0; i < qw.size(); i++) lines.push_back((i ? "    " : dot) + qw[i]);
    auto aw = wrap_plain(a, width - 6);
    for (size_t i = 0; i < aw.size(); i++)
      lines.push_back((i ? "      " : "    " + arrow) + blue + aw[i] + "\x1b[0m");
    return lines;
  }

  // system / header (1-column guard, see kind 1 above)
  auto w = wrap_plain(raw, width - 1);
  std::vector<std::string> lines;
  for (auto& l : w) lines.push_back("\x1b[2m\x1b[38;5;245m" + l + "\x1b[0m");
  if (lines.empty()) lines.push_back("");
  return lines;
}

std::vector<std::string> Ui::content_lines() const {
  int C = term_cols();
  std::vector<std::string> out;
  for (size_t i = 0; i < items_.size(); i++) {
    for (auto& l : items_[i].lines) out.push_back(l);
    // Answered questions (kind 7) group together: one block, no blank between them.
    bool group = i + 1 < items_.size() && items_[i].kind == 7 && items_[i + 1].kind == 7;
    if (!group && (i + 1 < items_.size() || reasoning_active_ || !live_.empty())) out.push_back("");
  }
  if (reasoning_active_ && !reasoning_.empty()) {
    for (auto& l : render_item(4, reasoning_, C)) out.push_back(l);
    out.push_back("");
  }
  if (!live_.empty()) {
    // Re-parse markdown only when the streamed text (or width) actually changed;
    // the spinner redraw otherwise just reuses the cached lines.
    std::string key = live_ + " " + std::to_string(C);
    if (key != live_cache_key_) {
      live_cached_ = render::markdown(live_, C - 1);
      live_cache_key_ = std::move(key);
    }
    auto& ml = live_cached_;
    size_t first = 0;
    while (first < ml.size() && ml[first].empty()) first++;
    for (size_t i = first; i < ml.size(); i++) out.push_back(ml[i]);
  }
  return out;
}

std::string Ui::status_string() const {
  std::string left;
  if (!status_override_.empty()) {
    left = status_override_;  // explicit transient status (e.g. compaction) wins
  } else if (working_ && !answer_started_) {
    long el = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - think_start_)
                   .count();
    size_t f = (((el / 80) % 10) + 10) % 10;
    left = std::string(kSpinner[f]);
  }
  return left;
}

std::string Ui::token_line_str() const {
  // Per-turn numbers = everything since the last user prompt (all rounds), plus
  // the in-progress round's live output estimate (reasoning + streamed text).
  long in_show = turn_in_;
  long out_show = turn_out_ + live_out_est_;
  long s_out = session_out_ + live_out_est_;
  return token_line(true, in_show, out_show, session_in_, s_out, ctx_used_, cfg_.context_window);
}

void Ui::redraw() {
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
  // The input box expands to fit the number of physical rows input_ wraps to
  // (multi-line via Shift+Enter / paste, plus long lines that wrap). It grows
  // upward so the bottom rule + token line stay pinned to the bottom.
  //
  // When the picker is active it takes the input box's place:
  // no box, no rules, and it draws its own caret, pinned to the bottom rows above
  // the token line. The input box and the attach line are then hidden.
  const bool box = !picker_active_;
  std::vector<std::string> plines;  // picker lines (empty when not active)
  if (!box) pick_lines(plines, C);
  std::vector<std::string> in_lines = box ? wrap_input(input_, C - 3) : std::vector<std::string>{};
  int nlines = (int)in_lines.size();
  std::string attach = box ? attach_line() : "";
  int has_attach = box && !attach.empty() ? 1 : 0;
  // Caret = end of the last row of the prefix (everything before the cursor).
  auto pre = wrap_input(input_.substr(0, cursor_), C - 3);
  int cur_line = (int)pre.size() - 1;
  int cur_col = render::display_width(pre.back());
  if (cur_line < 0) { cur_line = 0; cur_col = 0; }

  int top_rule_row;  // meaningful only in box mode
  int tr_h;  // transcript rows 0..tr_h-1; row tr_h is the blank gap (box) / picker start
  if (box) {
    top_rule_row = R - 3 - nlines - has_attach;
    if (top_rule_row < 1) top_rule_row = 1;
    tr_h = top_rule_row - 1;
    if (tr_h < 0) tr_h = 0;
  } else {
    top_rule_row = 0;
    int P = (int)plines.size();
    tr_h = R - 1 - P;  // picker occupies rows [tr_h, R-1); token line is row R-1
    if (tr_h < 0) tr_h = 0;
  }

  // Index of the transcript line drawn at row 0. It is negative when the
  // transcript is shorter than the area, which means blank top padding so the
  // transcript is bottom-anchored. `off` scrolls up within the transcript.
  long base = (long)tr.size() - (long)tr_h;
  long max_up = base > 0 ? base : 0;
  long off = scroll_offset_;
  if (off < 0) off = 0;
  // While scrolled up, keep the view pinned: as new lines append below the pinned
  // window they would otherwise push `first` down and shift everything on screen.
  // Offset them out of `off` so the top line stays put (scroll-back-to-bottom, i.e.
  // off==0, still follows the live tail).
  if (off > 0) {
    long growth = (long)tr.size() - last_tr_size_;
    if (growth > 0) off += growth;
  }
  last_tr_size_ = (long)tr.size();
  if (off > max_up) off = max_up;
  scroll_offset_ = off;  // clamp stored value so PgUp at top is a no-op
  long first = base - off;

  // Render one input line. The caret is the real terminal cursor (positioned at
  // the end of the frame), so this just draws the line plain.
  auto render_input_line = [&](const std::string& il, const std::string& prefix, bool, size_t) -> std::string {
    return prefix + il;
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
    } else if (!box && i >= tr_h && i < R - 1) {
      // Picker line: the picker is pinned to the bottom, above the token line.
      int pi = i - tr_h;
      if (pi >= 0 && pi < (int)plines.size())
        line = plines[pi];
    } else if (box) {
      if (i == top_rule_row || i == R - 2) {
        // Full-width horizontal rules around the input box (above and below it), dimmed.
        line.clear();
        line.reserve((size_t)C * 3 + 8);
        line += "\x1b[2m";
        for (int c = 0; c < C; c++) line += "\xe2\x94\x80";
        line += "\x1b[0m";
      } else if (i == R - 1) {
        line = tokens;
      } else if (has_attach && i == top_rule_row + 1) {
        // Attachment line (clipboard images), orange; trim to fit the width.
        std::string a = attach;
        while (render::display_width(a) > C - 2) {
          size_t cut = rune_before(a, a.size());
          a.erase(cut);
        }
        line = "\x1b[38;5;208m" + a + "\x1b[0m";
      } else if (i > top_rule_row + has_attach && i < R - 2) {
        int li = i - top_rule_row - 1 - has_attach;  // which input row this screen row holds
        if (li >= 0 && li < (int)in_lines.size())
          line = render_input_line(in_lines[li], li == 0 ? "> " : "  ", li == cur_line, 0);
      }
      // i == tr_h (the gap) and any overflow input rows are left blank.
    }
    // In picker mode, row R-1 is the token line and any overflow picker rows blank.
    if (!box && i == R - 1) line = tokens;
    frame += "\x1b[2K";  // clear whole row so a shorter line leaves no old tail
    frame += line;
    if (i != R - 1) frame += "\n";
  }

  // Caret placement. In box mode it is the real terminal cursor at the end of the
  // input row. In picker mode the picker draws its own caret (the "own answer"
  // Caret placement. In box mode the real terminal cursor sits in the input row.
  // In picker mode the "own answer" row is the only editable line: when it is the
  // active row the real terminal cursor sits in the typed text; otherwise it is
  // hidden (the "❯" glyph is the selector) so it never jumps between rows.
  int hw_row, cursor_col;
  bool caret_shown;
  if (box) {
    hw_row = top_rule_row + 2 + has_attach + cur_line;
    // 1-indexed: 2-char prefix + prefix width + 1. Clamp to C so the caret at the
    // end of a full-width row stays on the last column instead of wrapping off-screen.
    cursor_col = 3 + cur_col;
    caret_shown = true;
  } else {
    bool in_own_row = pick_caret_line_ >= 0;
    hw_row = tr_h + 1 + (in_own_row ? pick_caret_line_ : 0);  // +1 for 1-indexed CUP
    cursor_col = in_own_row ? pick_caret_col_ + 1 : 1;  // 1-indexed
    caret_shown = in_own_row;  // hide it otherwise: the "❯" is the selector
  }
  if (hw_row < 1) hw_row = 1;
  if (hw_row > R - 1) hw_row = R - 1;
  if (cursor_col > C) cursor_col = C;
  frame += "\x1b[" + std::to_string(hw_row) + ";" + std::to_string(cursor_col) + "H";
  frame += caret_shown ? "\x1b[?25h" : "\x1b[?25l";  // show only in the editable row

  if (frame == last_frame_ && R == last_rows_ && C == last_cols_) {
    std::cout.flush();
    return;
  }
  last_frame_ = frame;
  last_rows_ = R;
  std::cout << frame << std::flush;
}

// --- Agent-facing hooks ---

void Ui::on_model_start() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  working_ = true;
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

void Ui::on_reasoning(const std::string& c) {
  if (c.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  reasoning_active_ = true;
  for (char ch : c) reasoning_ += (ch == '\n' || ch == '\r') ? ' ' : ch;
  live_out_est_ = (live_.size() + reasoning_.size()) / 4;
  if (interactive_) {
    // Interactive: the spinner thread re-renders on its next tick; a synchronous
    // redraw would hold draw_mu_ on the agent thread and stall input polling.
  } else {
    one_shot_thinking_line();
  }
}

void Ui::on_text(const std::string& c) {
  if (c.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  answer_started_ = true;
  live_ += c;
  live_out_est_ = (live_.size() + reasoning_.size()) / 4;
  // Do NOT redraw here: the spinner thread owns redraw cadence and re-renders on
  // its next tick (the markdown is cached, so that is cheap). Holding draw_mu_
  // per streamed token on the agent thread would stall input polling.
  // (one-shot: buffered, rendered on round_end to keep markdown clean.)
}

void Ui::on_round_end() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  // Re-arm the spinner in case the next round runs tools; on_turn_end turns it off.
  answer_started_ = false;
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

void Ui::on_turn_end() {
  std::lock_guard<std::mutex> lk(draw_mu_);
  working_ = false;
  if (interactive_) redraw();
}

void Ui::on_tool_start(const std::string& name, const std::string& args) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  if (interactive_) redraw();
}

void Ui::on_tool_call(const std::string& name, const std::string& args, const std::string& result) {
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

void Ui::on_system(const std::string& msg) {
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

void Ui::on_usage(long in, long out, long total) {
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

void Ui::on_status(const std::string& msg) {
  std::lock_guard<std::mutex> lk(draw_mu_);
  status_override_ = msg;
  if (interactive_) {
    redraw();
  } else if (!msg.empty()) {
    std::cout << (is_tty_out() ? "\x1b[2m" + msg + "\x1b[0m" : msg) << "\n";
    std::cout.flush();
  }
}

void Ui::on_images(const std::vector<ImagePart>& images) {
  if (images.empty()) return;
  std::lock_guard<std::mutex> lk(draw_mu_);
  int C = term_cols();
  Item it;
  it.kind = 6;
  for (size_t i = 0; i < images.size(); i++) {
    if (i) it.raw += "  ";
    it.raw += (images[i].mime.empty() ? "image" : images[i].mime) +
              " \xc2\xb7 " + fmt_bytes(images[i].data.size());
  }
  it.lines = render_item(6, it.raw, C);
  items_.push_back(std::move(it));
  if (interactive_) {
    redraw();
  } else {
    std::cout << (is_tty_out() ? it.lines.front() : render::strip_ansi(it.lines.front())) << "\n";
    std::cout.flush();
  }
}

bool Ui::confirm(const std::string& plan) {
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

// --- Picker / ask_user ---

void Ui::hint(const std::string& msg) {
  hint_ = msg;
  hint_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
}

bool Ui::submit_tab() const {
  return asking_ && ask_qs_.size() > 1 && ask_idx_ == ask_qs_.size();
}
bool Ui::on_own_row() const {
  return asking_ && !submit_tab() && pick_sel_ == pick_opts_.size();
}
bool Ui::on_submit_row() const {
  return asking_ && pick_multi_ && !submit_tab() && pick_sel_ == pick_opts_.size() + 1;
}
size_t Ui::pick_rows() const {
  if (submit_tab()) return 2;  // "Submit answers" / "Cancel"
  if (!asking_) return pick_opts_.size();
  return pick_opts_.size() + (pick_multi_ ? 2 : 1);  // options + "own" (+ "Submit")
}

void Ui::pick_finish(int r) {
  pick_result_ = r;
  picker_active_ = false;
  asking_ = false;
  ask_cv_.notify_all();
}

void Ui::ask_show(size_t idx) {
  if (ask_idx_ < ask_qs_.size()) {  // keep the tab we leave as it was
    auto& st = ask_st_[ask_idx_];
    st.sel = pick_sel_;
    st.marked = pick_marked_;
    st.typed = input_;
  }
  ask_idx_ = idx;
  if (idx < ask_qs_.size()) {
    auto& st = ask_st_[idx];
    pick_title_ = ask_qs_[idx].question;
    pick_opts_ = ask_qs_[idx].opts;
    pick_multi_ = ask_qs_[idx].multi;
    pick_sel_ = st.sel;
    if (st.marked.size() != pick_opts_.size()) st.marked.assign(pick_opts_.size(), false);
    pick_marked_ = st.marked;
    input_ = st.typed;
  } else {  // Submit tab
    pick_opts_.clear();
    pick_multi_ = false;
    pick_sel_ = 0;
    input_.clear();
  }
  cursor_ = input_.size();
}

void Ui::ask_confirm() {
  std::string a;
  if (pick_multi_) {  // checked options, plus the typed answer if there is one
    for (size_t i = 0; i < pick_opts_.size(); i++)
      if (pick_marked_[i]) a += (a.empty() ? "" : ", ") + pick_opts_[i].label;
    std::string own = util::trim(input_);
    if (!own.empty()) a += (a.empty() ? "" : ", ") + own;
    if (a.empty()) {
      hint("check at least one, then Submit");
      return;
    }
  } else if (on_own_row()) {
    a = util::trim(input_);
    if (a.empty()) {
      hint("type your answer, or pick an option");
      return;
    }
  } else if (pick_sel_ < pick_opts_.size()) {
    a = pick_opts_[pick_sel_].label;
  }
  ask_st_[ask_idx_].answer = a;
  size_t nq = ask_qs_.size();
  if (nq == 1) {
    pick_finish(0);
    return;
  }
  ask_show(ask_idx_ + 1);  // the next question, or the Submit tab after the last
}

bool Ui::ask_key(int c) {
  size_t nq = ask_qs_.size();
  if (c == 0x03) {
    pick_finish(-1);
    return true;
  }
  if (c == '\t') {  // next tab; tabs do not wrap
    if (nq > 1 && ask_idx_ < nq) {
      ask_show(ask_idx_ + 1);
      return true;
    }
    return true;
  }
  if (submit_tab()) {
    if (c == '1' || c == '2') pick_sel_ = (size_t)(c - '1');
    if (c == '\r' || c == '1' || c == '2') pick_finish(pick_sel_ == 0 ? 0 : -1);
    return c != 0x1b;  // Esc still interrupts
  }
  if (c == '\r') {
    if (pick_multi_ && pick_sel_ < pick_opts_.size())
      pick_marked_[pick_sel_] = !pick_marked_[pick_sel_];  // Enter checks; Submit confirms
    else
      ask_confirm();
    return true;
  }
  if (!on_own_row() && c >= '1' && c <= '9' && (size_t)(c - '1') < pick_rows()) {
    pick_sel_ = (size_t)(c - '1');
    if (on_own_row()) return true;  // now type the answer
    if (on_submit_row()) {
      ask_confirm();
    } else if (pick_multi_) {
      pick_marked_[pick_sel_] = !pick_marked_[pick_sel_];
    } else {
      ask_confirm();
    }
    return true;
  }
  if (pick_multi_ && c == ' ' && !on_own_row() && pick_sel_ < pick_opts_.size()) {
    pick_marked_[pick_sel_] = !pick_marked_[pick_sel_];
    return true;
  }
  // Anything else printable is the start of the user's own answer.
  if (c >= 0x20 && c != 0x7f && !on_own_row()) pick_sel_ = pick_opts_.size();
  return false;  // editing keys go to the input box
}

bool Ui::picker_key(int c) {
  if (asking_) return ask_key(c);
  return false;
}

// Draw the picker into `out` (top-down, one element per screen row) and set
// pick_caret_line_/col_ to the typed row's caret (line index in `out`, -1 if none).
void Ui::pick_lines(std::vector<std::string>& out, int C) {
  pick_caret_line_ = -1;
  pick_caret_col_ = 0;
  const std::string dim = "\x1b[2m", bold = "\x1b[1m", blue = "\x1b[38;5;153m";
  int W = std::max(10, C - 1);
  auto clip = [&](const std::string& s) {  // cut to W columns on a rune boundary
    if (render::display_width(render::strip_ansi(s)) <= W) return s + "\x1b[0m";
    std::string plain = render::strip_ansi(s), keep;
    for (size_t i = 0; i < plain.size();) {
      size_t j = rune_after(plain, i);
      if (render::display_width(keep + plain.substr(i, j - i)) > W - 1) break;
      keep += plain.substr(i, j - i);
      i = j;
    }
    return keep + "\xe2\x80\xa6\x1b[0m";
  };
  std::string rule;
  for (int i = 0; i < C; i++) rule += "\xe2\x94\x80";
  std::string color = asking_ ? "\x1b[38;5;240m" : "\x1b[38;5;75m";
  out.push_back(color + rule + "\x1b[0m");

  if (asking_ && ask_qs_.size() > 1) {  // ← ☐ Q1  ☐ Q2  ✓ Submit →
    std::string tabs = dim + "\xe2\x86\x90\x1b[0m ";
    for (size_t i = 0; i <= ask_qs_.size(); i++) {
      bool last = i == ask_qs_.size();
      std::string name = last ? "Submit"
                              : (ask_qs_[i].header.empty()
                                     ? "Q" + std::to_string(i + 1)
                                     : ask_qs_[i].header);
      std::string boxc = last
                             ? "\x1b[38;5;141m\xe2\x9c\x93\x1b[0m"
                             : (ask_st_[i].answer.empty() ? "\xe2\x98\x90" : "\xe2\x98\x92");
      if (i == ask_idx_)
        tabs += "\x1b[48;5;153m\x1b[38;5;16m " + render::strip_ansi(boxc) + " " + name + " \x1b[0m ";
      else
        tabs += " " + boxc + " " + name + "  ";
    }
    out.push_back(clip(tabs + dim + "\xe2\x86\x92"));
    out.push_back("");
  }

  if (submit_tab()) {
    out.push_back(clip(bold + "Review your answers"));
    out.push_back("");
    bool all = true;
    for (size_t i = 0; i < ask_qs_.size(); i++) {
      const std::string& a = ask_st_[i].answer;
      if (a.empty()) {
        all = false;
        continue;
      }
      out.push_back(clip(" \xe2\x97\x8f " + ask_qs_[i].question));
      out.push_back(clip("   " + blue + "\xe2\x86\x92 " + a));
    }
    if (!all) {
      if (!out.back().empty()) out.push_back("");
      out.push_back(clip("\x1b[38;5;220m\xe2\x9a\xa0 You have not answered all questions"));
    }
    out.push_back("");
    out.push_back(clip(dim + "Ready to submit your answers?"));
    out.push_back("");
    const char* rows[] = {"Submit answers", "Cancel"};
    for (size_t i = 0; i < 2; i++)
      out.push_back(clip((i == pick_sel_ ? blue + "\xe2\x9d\xaf " : std::string("  ")) + dim +
                         std::to_string(i + 1) + ".\x1b[0m " + (i == pick_sel_ ? blue : "") + rows[i]));
  } else if (asking_) {
    for (auto& l : wrap_plain(pick_title_, W)) out.push_back(clip(bold + l));
    out.push_back("");
    size_t n = pick_rows();
    for (size_t i = 0; i < n; i++) {
      bool sel = i == pick_sel_;
      bool own = i == pick_opts_.size();
      bool submit = i == pick_opts_.size() + 1;
      std::string num = submit ? "   " : std::to_string(i + 1) + ".";
      std::string boxc;
      if (pick_multi_ && !submit)
        boxc = (own ? !util::trim(input_).empty() : pick_marked_[i]) ? "[\xe2\x9c\x93] " : "[ ] ";
      std::string lead = (sel ? blue + "\xe2\x9d\xaf\x1b[0m " : std::string("  ")) + dim + num +
                         "\x1b[0m " + boxc;
      int lead_w = render::display_width(render::strip_ansi(lead));  // strip ANSI first
      if (own) {
        // The "own answer" row is an editable line. Render it with wrap_input (the
        // caret-safe wrapper the input box uses) and let the real terminal cursor
        // sit in it; an inline inverse caret misaligns and desyncs the spacing.
        std::vector<std::string> own_lines = wrap_input(input_, W - lead_w);
        for (size_t k = 0; k < own_lines.size() && k < 3; k++) {
          std::string text = own_lines[k];
          if (input_.empty() && k == 0)
            text += "\x1b[2mType something.";  // placeholder only when nothing typed
          out.push_back(clip((k ? std::string((size_t)lead_w, ' ') : lead) + text));
        }
        if (sel && !own_lines.empty()) {
          // Caret on the row the cursor byte falls in, at the column after it.
          auto pre = wrap_input(input_.substr(0, cursor_), W - lead_w);
          size_t cl = std::min(pre.size(), own_lines.size()) - 1;
          pick_caret_line_ = (int)out.size() - (int)(own_lines.size()) + (int)cl;
          int lead_plain = render::display_width(render::strip_ansi(lead));
          pick_caret_col_ = lead_plain + render::display_width(pre.back());
        }
      } else {
        std::string label = submit ? "Submit" : pick_opts_[i].label;
        auto lw = wrap_plain(label, W - lead_w);
        for (size_t k = 0; k < lw.size() && k < 3; k++) {
          out.push_back(clip((k ? std::string((size_t)lead_w, ' ') : lead) +
                             (sel ? blue : "") + lw[k]));
        }
      }
      if (!own && !submit && !pick_opts_[i].desc.empty()) {
        auto dw = wrap_plain(pick_opts_[i].desc, W - lead_w);
        for (size_t k = 0; k < dw.size() && k < 3; k++)
          out.push_back(clip(std::string((size_t)lead_w, ' ') + dim + dw[k]));
      }
    }
  }
  out.push_back("");
  std::string help = asking_
                         ? (on_own_row()
                                ? "Type your answer \xc2\xb7 Enter to select \xc2\xb7 Ctrl-C to cancel"
                                : "Enter to select \xc2\xb7 Tab/Arrows to navigate \xc2\xb7 Ctrl-C to cancel")
                         : "Enter to select \xc2\xb7 Ctrl-C to cancel";
  out.push_back(clip("\x1b[38;5;245m" + help));
}

std::vector<std::string> Ui::ask(const std::vector<Question>& qs) {
  if (!interactive_) return {};  // headless: nothing to ask
  std::unique_lock<std::mutex> lk(draw_mu_);
  std::string saved = input_;  // a typed-ahead draft must not become the answer
  size_t saved_cursor = cursor_;
  ask_qs_ = qs;
  ask_st_.assign(qs.size(), AskState{});
  for (size_t i = 0; i < qs.size(); i++) ask_st_[i].marked.assign(qs[i].opts.size(), false);
  asking_ = true;
  ask_idx_ = qs.size();  // nothing to save yet
  ask_show(0);
  pick_result_ = -2;
  picker_active_ = true;
  hint_.clear();
  status_override_ = "\x1b[38;5;220mwaiting for your answer\x1b[0m";
  redraw();
  while (pick_result_ == -2 && !util::interrupted())
    ask_cv_.wait_for(lk, std::chrono::milliseconds(100));
  picker_active_ = false;
  asking_ = false;
  status_override_.clear();
  std::vector<std::string> answers;
  if (pick_result_ >= 0) {
    int C = term_cols();
    for (size_t i = 0; i < qs.size(); i++) {
      answers.push_back(ask_st_[i].answer);
      Item it;
      it.kind = 7;
      it.raw = qs[i].question + "\t" + ask_st_[i].answer;
      it.lines = render_item(7, it.raw, C);
      items_.push_back(std::move(it));
    }
  }
  pick_opts_.clear();
  input_ = saved;
  cursor_ = saved_cursor;
  redraw();
  return answers;
}

// --- Interactive input ---

void Ui::user_message(const std::string& text) {
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
  scroll_offset_ = 0;
  redraw();
}

void Ui::clear_transcript() {
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

std::string Ui::read_input() {
  std::unique_lock<std::mutex> lk(draw_mu_);
  working_ = false;  // idle prompt: no turn in progress
  for (;;) {
    int c = 0;
    // Release the lock while blocking on stdin so the spinner thread can
    // still acquire it and redraw on resize.
    lk.unlock();
    bool ok = read_one_byte(&c);
    lk.lock();
    if (!ok) {
      exit_requested_ = true;
      std::string s = input_;
      input_.clear();
      cursor_ = 0;
      redraw();
      return s;
    }
    // Terminal-submit keys live here (physical Enter submits; Ctrl-C clears or
    // exits; Ctrl-D exits or deletes). Everything else is editing and is handled
    // by the SAME shared core used by the live-turn poll, so every keybinding
    // behaves identically at the idle prompt and while the agent is working.
    if (c == '\r') {
      std::string s = input_;
      hist_idx_ = -1;  // a submit leaves history navigation
      suspended_input_.clear();
      if (!s.empty() && (history_.empty() || history_.back() != s))
        history_.push_back(s);  // in-memory only; skip consecutive duplicates
      input_.clear();
      cursor_ = 0;
      redraw();
      return s;
    } else if (c == 0x03) {  // Ctrl-C: clear line + attached images, or exit if empty
      if (!input_.empty() || !pending_images_.empty()) {
        input_.clear();
        cursor_ = 0;
        pending_images_.clear();
      } else {
        exit_requested_ = true;
        std::string s = input_;
        input_.clear();
        redraw();
        return s;
      }
    } else if (c == 0x04) {  // Ctrl-D: exit if empty, else delete one char
      if (input_.empty()) {
        exit_requested_ = true;
        std::string s = input_;
        input_.clear();
        redraw();
        return s;
      }
      size_t to = rune_after(input_, cursor_);
      input_.erase(cursor_, to - cursor_);
    } else if (c == 0x16) {  // Ctrl-V: paste clipboard (image -> attach, else text)
      lk.unlock();
      ClipData clip = read_clipboard();
      lk.lock();
      if (clip.is_image) {
        attach_image(clip);
      } else {
        input_.insert(cursor_, clip.text);
        cursor_ += clip.text.size();
      }
    } else {
      apply_edit_key(c);  // shared editing core (identical to live-turn poll)
    }
    redraw();
  }
}

void Ui::consume_paste(std::string pre) {
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

bool Ui::stdin_ready(int ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}

// Shared editing core (cursor moves / kill ring / yank / meta keys / printable /
// backspace / ESC sequences). Identical for the idle prompt and the live-turn
// poll so every keybinding works in both modes. Caller holds draw_mu_. The leading
// ESC, when present, has already been consumed by the caller's read_one_byte.
bool Ui::apply_edit_key(int c) {
  if (c == 0x7f || c == 0x08) {  // Backspace / DEL
    if (cursor_ > 0) {
      size_t from = rune_before(input_, cursor_);
      input_.erase(from, cursor_ - from);
      cursor_ = from;
    }
    return true;
  } else if (c == 0x15) {  // Ctrl-U: kill to start of line
    input_.erase(0, cursor_);
    cursor_ = 0;
    return true;
  } else if (c == 0x01) {  // Ctrl-A: beginning of line
    if (cursor_ != 0) { cursor_ = 0; return true; }
    return false;
  } else if (c == 0x02) {  // Ctrl-B: backward one rune
    if (cursor_ > 0) { cursor_ = rune_before(input_, cursor_); return true; }
    return false;
  } else if (c == 0x05) {  // Ctrl-E: end of line
    if (cursor_ != input_.size()) { cursor_ = input_.size(); return true; }
    return false;
  } else if (c == 0x06) {  // Ctrl-F: forward one rune
    if (cursor_ < input_.size()) { cursor_ = rune_after(input_, cursor_); return true; }
    return false;
  } else if (c == 0x0b) {  // Ctrl-K: kill to end of line
    kill_ring_ = input_.substr(cursor_);
    input_.erase(cursor_);
    return true;
  } else if (c == 0x19) {  // Ctrl-Y: yank kill ring
    input_.insert(cursor_, kill_ring_);
    cursor_ += kill_ring_.size();
    return true;
  } else if (c == 0x10) {  // Ctrl-P: history up (older)
    history_nav(-1);
    return true;
  } else if (c == 0x0e) {  // Ctrl-N: history down (newer)
    history_nav(+1);
    return true;
  } else if (c == 0x0c) {  // Ctrl-L: snap back to the bottom of the transcript
    scroll_offset_ = 0;
    return true;
  } else if (c == 0x0f) {  // Ctrl-O: toggle thinking visibility
    show_reasoning_ = !show_reasoning_;
    int C = term_cols();
    for (auto& it : items_)
      if (it.kind == 4) it.lines = render_item(4, it.raw, C);
    return true;
  } else if (c == 0x1b) {  // Escape: parse the rest of the sequence
    return apply_esc_bytes();
  } else if (c == '\n') {  // pasted newline: insert literally (only Enter submits)
    input_.insert(cursor_, 1, '\n');
    cursor_++;
    return true;
  } else if (c >= 0x20) {  // printable (incl. high UTF-8 bytes)
    input_.insert(cursor_, 1, (char)c);
    cursor_++;
    return true;
  }
  return false;
}

bool Ui::history_nav(int dir) {
  if (dir < 0) {  // Up: go older (toward newest first). Start from current buffer.
    if (history_.empty()) return false;
    if (hist_idx_ == -1) {  // begin navigating: save the free buffer
      suspended_input_ = input_;
      hist_idx_ = (int)history_.size() - 1;
    } else if (hist_idx_ > 0) {
      hist_idx_--;
    } else {
      return false;  // already at the newest
    }
    input_ = history_[hist_idx_];
    cursor_ = input_.size();
    return true;
  }
  // Down: go newer (toward current/freest).
  if (hist_idx_ == -1) return false;  // already editing freely
  if (hist_idx_ < (int)history_.size() - 1) {
    hist_idx_++;
    input_ = history_[hist_idx_];
    cursor_ = input_.size();
    return true;
  }
  // Past the newest: restore the free buffer and stop navigating.
  hist_idx_ = -1;
  input_ = suspended_input_;
  cursor_ = input_.size();
  suspended_input_.clear();
  return true;
}

// Parse an ANSI escape sequence whose leading ESC has already been consumed.
// Handles bracketed paste, emacs meta keys (M-f / M-b / M-d), Shift+Enter, SGR
// mouse, and cursor/scroll sequences. Caller holds draw_mu_. Returns true if the
// input box or scroll position changed.
bool Ui::apply_esc_bytes() {
  unsigned char buf[64] = {0};
  int n = 0;
  // Wait for the byte after ESC; if none, it was a lone ESC or meta key.
  // For CSI/SS3 ('[' / 'O'), block-read until the final byte so the sequence
  // is always consumed whole even if the terminal spaces the bytes out.
  if (!stdin_ready(10)) return 0;
  unsigned char b;
  ssize_t r = read(STDIN_FILENO, &b, 1);
  if (r != 1) return 0;
  buf[n++] = b;
  if (b == '[' || b == 'O') {                    // real CSI/SS3: block until the final byte
    for (;;) {
      if (n >= (int)sizeof(buf) - 1) break;      // overflow guard
      unsigned char c;
      if (read(STDIN_FILENO, &c, 1) != 1) break;
      buf[n++] = c;
      if (c >= 0x40 && c <= 0x7E && c != '[' && c != 'O') break;  // final byte: done
    }
  }
  // 2-byte case (ESC + letter = meta key, or ESC + control char): no more bytes to
  // drain -- fall through to the meta-key handler (M-f / M-b / M-d) below.
  bool changed = false;
  auto scroll_up = [&]() { scroll_offset_++; changed = true; };
  auto scroll_down = [&]() { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; changed = true; };
  auto fwd = [&]() { if (cursor_ < input_.size()) { cursor_ = rune_after(input_, cursor_); changed = true; } };
  auto back = [&]() { if (cursor_ > 0) { cursor_ = rune_before(input_, cursor_); changed = true; } };
  int i = 0;
  if (n >= 5 && buf[0] == '[' && buf[1] == '2' && buf[2] == '0' && buf[3] == '0' && buf[4] == '~') {
    // Bracketed paste start (ESC[200~ ... ESC[201~): insert blob literally so
    // pasted newlines never submit the prompt; only a physical Enter does.
    std::string pre((const char*)&buf[5], (size_t)(n - 5));
    consume_paste(std::move(pre));
    i = n;
    changed = true;
  }
  // Meta (ESC) + a normal character: emacs-style M-key (ESC already consumed).
  if (i == 0 && n >= 1) {
    unsigned char mc = buf[0];
    if (mc < 0x20 && !(mc == '[' || mc == 'O')) {
      i = n;  // ESC + control char: ignore
    } else if (mc == 'f') {
      if (cursor_ < input_.size()) cursor_ = word_end(input_, cursor_);
      i = n;
      changed = true;
    } else if (mc == 'b') {
      if (cursor_ > 0) cursor_ = word_start(input_, cursor_);
      i = n;
      changed = true;
    } else if (mc == 'd') {
      size_t e = word_end(input_, cursor_);
      if (e > cursor_) {
        kill_ring_ = input_.substr(cursor_, e - cursor_);
        input_.erase(cursor_, e - cursor_);
      }
      i = n;
      changed = true;
    }
  }
  // First sequence (no leading ESC in buffer).
  if (i == 0 && n >= 2) {
    if (n == 9 && buf[0] == '[' && buf[1] == '2' && buf[2] == '7' &&
        buf[3] == ';' && buf[4] == '2' && buf[5] == ';' && buf[6] == '1' &&
        buf[7] == '3' && buf[8] == '~') {
      input_.insert(cursor_, 1, '\n');  // Shift+Enter (ESC[27;2;13~)
      cursor_++;
      i = n;
      changed = true;
    } else if (buf[0] == '[' && buf[1] == '<') {  // SGR mouse: [<Btn;Col;RowM|m>
      i = 2;
      int btn = 0;
      while (i < n && buf[i] >= '0' && buf[i] <= '9') { btn = btn * 10 + (buf[i] - '0'); i++; }
      if (btn == 64) scroll_up();
      else if (btn == 65) scroll_down();
      while (i < n && buf[i] != 0x1b) i++;
    } else if (buf[0] == '[') {
      char p = (char)buf[1]; i = 2;
      if ((p == 'A' || p == 'B') && picker_active_ && pick_rows() > 0) {
        size_t n = pick_rows();  // Up/Down move the picker, not the transcript
        pick_sel_ = p == 'A' ? (pick_sel_ + n - 1) % n : (pick_sel_ + 1) % n;
        changed = true;
      }
      else if (p == 'A') scroll_up();
      else if (p == 'B') scroll_down();
      else if (p == 'D') back();
      else if (p == 'C') fwd();
      else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
      else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
      else if (p == '5') scroll_up();
      else if (p == '6') scroll_down();
      else if (p >= '0' && p <= '9') {
        int num = p - '0';
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { num = num * 10 + (buf[i] - '0'); i++; }
        if (i < n && buf[i] == ';') {
          if (num == 64) scroll_up();
          else if (num == 65) scroll_down();
        }
      }
      while (i < n && buf[i] != 0x1b) i++;
    } else if (buf[0] == 'O') {
      char p = (char)buf[1]; i = 2;
      if (p == 'D') back();
      else if (p == 'C') fwd();
      else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
      else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
    }
  }
  // Remaining sequences (each starts with ESC).
  while (i < n) {
    if (buf[i] == 0x1b && i + 1 < n && buf[i + 1] == '[') {
      i += 2;
      if (i < n && buf[i] == '<') {
        i++;
        int btn = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { btn = btn * 10 + (buf[i] - '0'); i++; }
        if (btn == 64) scroll_up();
        else if (btn == 65) scroll_down();
        while (i < n && buf[i] != 0x1b) i++;
      } else if (i < n) {
        char p = (char)buf[i]; i++;
        if (p == 'A') scroll_up();
        else if (p == 'B') scroll_down();
        else if (p == 'D') back();
        else if (p == 'C') fwd();
        else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
        else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
        else if (p == '5') scroll_up();
        else if (p == '6') scroll_down();
        while (i < n && buf[i] != 0x1b) i++;
      }
    } else if (buf[i] == 0x1b && i + 1 < n && buf[i + 1] == 'O') {
      i += 2;
      if (i < n) {
        char p = (char)buf[i]; i++;
        if (p == 'D') back();
        else if (p == 'C') fwd();
        else if (p == 'H') { if (cursor_ != 0) { cursor_ = 0; changed = true; } }
        else if (p == 'F') { if (cursor_ != input_.size()) { cursor_ = input_.size(); changed = true; } }
      }
      while (i < n && buf[i] != 0x1b) i++;
    } else {
      i++;
    }
  }
  return changed;
}

// Apply a single keystroke read during a live turn to the input box. Returns the
// poll outcome (kCancel on Ctrl-C, kRedraw if the box changed). Delegates all
// editing to apply_edit_key so it stays in lockstep with the idle prompt.
Ui::PollResult Ui::handle_poll_byte(int c) {
  if (c == 0x03) {  // Ctrl-C: request a clean interrupt of the running turn
    util::request_interrupt();
    return PollResult::kCancel;
  }
  {
    std::lock_guard<std::mutex> lk(draw_mu_);
    if (picker_active_) {  // ask_user is waiting: keys answer the question
      bool changed = picker_key(c);
      if (!changed)
        changed = apply_edit_key(c);  // editing the typed answer
      redraw();
      return PollResult::kRedraw;
    }
  }
  if (c == 0x16) {  // Ctrl-V: paste clipboard (image -> attach, else text)
    ClipData clip = read_clipboard();
    std::lock_guard<std::mutex> lk(draw_mu_);
    if (clip.is_image) {
      attach_image(clip);
    } else {
      input_.insert(cursor_, clip.text);
      cursor_ += clip.text.size();
    }
    return PollResult::kRedraw;
  }
  std::lock_guard<std::mutex> lk(draw_mu_);
  bool changed = apply_edit_key(c);
  if (changed) {
    redraw();
    return PollResult::kRedraw;
  }
  return PollResult::kNone;
}

Ui::PollResult Ui::poll_input() {
  // Drive cadence off stdin readiness (tiny 1ms wait) instead of a fixed sleep,
  // and drain EVERY available keystroke per wake so rapid typing is applied
  // immediately rather than one byte per tick. This is what keeps the input box
  // fully responsive while the agent is working in the background.
  PollResult last = PollResult::kNone;
  for (;;) {
    if (!stdin_ready(1))
      return last;  // no pending input: yield so the outer loop can re-check
      // whether the agent finished and re-enter the prompt.
    int c = 0;
    if (!read_one_byte(&c)) return last;
    PollResult r = handle_poll_byte(c);
    if (r == PollResult::kCancel) return PollResult::kCancel;
    if (r == PollResult::kRedraw) last = PollResult::kRedraw;
    // loop back: any more bytes are waiting -> read them right now (no delay).
  }
}

void Ui::spinner_loop() {
  while (spin_run_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::lock_guard<std::mutex> lk(draw_mu_);
    redraw();
  }
}

void Ui::one_shot_thinking_line() {
  if (!is_tty_out()) return;
  int C = term_cols();
  long el = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - think_start_)
                  .count();
  std::cout << "\r\x1b[K" << spinner_thinking(reasoning_, el, C) << std::flush;
  one_shot_line_ = true;
}

void Ui::one_shot_thinking_clear() {
  if (one_shot_line_) {
    std::cout << "\r\x1b[K\n";
    one_shot_line_ = false;
    std::cout.flush();
  }
}

}  // namespace dog
