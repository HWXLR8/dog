#include "ui/markdown.hpp"
#include <cctype>
#include "util.hpp"

namespace dog::render {

namespace {

struct Style {
  bool bold = false, italic = false, code = false, link = false, dim = false;
  int color = -1;
};

struct SC {
  std::string bytes;
  int width = 0;
  Style st;
};

bool style_equal(const Style& a, const Style& b) {
  return a.bold == b.bold && a.italic == b.italic && a.code == b.code &&
         a.link == b.link && a.dim == b.dim && a.color == b.color;
}

std::string ansi_open(const Style& s) {
  std::string o;
  if (s.bold) o += "\x1b[1m";
  if (s.italic) o += "\x1b[3m";
  if (s.dim) o += "\x1b[2m";
  if (s.code) o += "\x1b[38;5;117m";
  if (s.link) {
    o += "\x1b[4m";
    o += "\x1b[38;5;75m";
  }
  if (s.color >= 0) o += "\x1b[38;5;" + std::to_string(s.color) + "m";
  return o;
}

std::string render_sc(const std::vector<SC>& scs) {
  std::string out;
  bool have = false;
  Style prev;
  for (auto& c : scs) {
    if (!have || !style_equal(prev, c.st)) {
      if (have) out += "\x1b[0m";
      out += ansi_open(c.st);
      prev = c.st;
      have = true;
    }
    out += c.bytes;
  }
  if (have) out += "\x1b[0m";
  return out;
}

int rune_width(unsigned cp) {
  if (cp == 0) return 0;
  if (cp >= 0x200B && cp <= 0x200F) return 0;  // zero-width joiners / combining
  if (cp == 0x2009) return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
      (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
      (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x2FFFD))
    return 2;
  return 1;
}

size_t utf8_decode(const std::string& s, size_t i, unsigned* cp) {
  if (i >= s.size()) return 0;
  unsigned char b = static_cast<unsigned char>(s[i]);
  if (b < 0x80) {
    *cp = b;
    return 1;
  }
  if ((b & 0xE0) == 0xC0) {
    if (i + 1 >= s.size()) return 0;
    *cp = ((b & 0x19) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    return 2;
  }
  if ((b & 0xF0) == 0xE0) {
    if (i + 2 >= s.size()) return 0;
    *cp = ((b & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    return 3;
  }
  if ((b & 0xF8) == 0xF0) {
    if (i + 3 >= s.size()) return 0;
    *cp = ((b & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    return 4;
  }
  *cp = b;
  return 1;
}

void append_run(std::vector<SC>& out, const std::string& bytes, const Style& st) {
  size_t i = 0;
  unsigned cp;
  size_t len;
  while ((len = utf8_decode(bytes, i, &cp)) > 0) {
    SC sc;
    sc.bytes = bytes.substr(i, len);
    sc.width = rune_width(cp);
    sc.st = st;
    out.push_back(sc);
    i += len;
  }
}

std::vector<SC> parse_inline(const std::string& line, const Style& initial) {
  std::vector<SC> out;
  Style cur = initial;
  size_t i = 0;
  auto emit_rune = [&](size_t pos) {
    unsigned cp;
    size_t len = utf8_decode(line, pos, &cp);
    if (len == 0) len = 1;
    SC sc;
    sc.bytes = line.substr(pos, len);
    sc.width = rune_width(cp);
    sc.st = cur;
    out.push_back(sc);
    return len;
  };
  while (i < line.size()) {
    char c = line[i];
    if (cur.code) {
      if (c == '`') {
        cur.code = false;
        i++;
        continue;
      }
      i += emit_rune(i);
      continue;
    }
    if (c == '`') {
      cur.code = true;
      i++;
      continue;
    }
    if (c == '*' && i + 1 < line.size() && line[i + 1] == '*') {
      cur.bold = !cur.bold;
      i += 2;
      continue;
    }
    if (c == '*' || c == '_') {
      cur.italic = !cur.italic;
      i++;
      continue;
    }
    if (c == '[') {
      size_t close = line.find(']', i);
      if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(') {
        size_t paren = line.find(')', close + 2);
        if (paren != std::string::npos) {
          Style save = cur;
          cur = Style();
          cur.link = true;
          size_t k = i;
          while (k < close) k += emit_rune(k);
          cur = save;
          i = paren + 1;
          continue;
        }
      }
      i += emit_rune(i);
      continue;
    }
    i += emit_rune(i);
  }
  return out;
}

std::vector<std::string> hard_break(const std::vector<SC>& word, int width) {
  std::vector<std::string> out;
  std::vector<SC> piece;
  int w = 0;
  for (auto& c : word) {
    if (w + c.width > width) {
      if (!piece.empty()) {
        out.push_back(render_sc(piece));
        piece.clear();
        w = 0;
      }
      if (c.width > width) {
        piece.push_back(c);
        out.push_back(render_sc(piece));
        piece.clear();
        w = 0;
        continue;
      }
    }
    piece.push_back(c);
    w += c.width;
  }
  if (!piece.empty()) out.push_back(render_sc(piece));
  if (out.empty()) out.push_back("");
  return out;
}

std::vector<std::string> wrap(const std::vector<SC>& chars, int width) {
  if (width <= 0) width = 80;
  // Split into words (groups of non-space chars).
  std::vector<std::vector<SC>> words;
  std::vector<SC> w;
  for (auto& c : chars) {
    if (c.bytes == " ") {
      if (!w.empty()) {
        words.push_back(w);
        w.clear();
      }
    } else {
      w.push_back(c);
    }
  }
  if (!w.empty()) words.push_back(w);

  std::vector<std::string> lines;
  std::vector<SC> cur;
  int curw = 0;
  auto flush = [&]() {
    if (!cur.empty()) lines.push_back(render_sc(cur));
    cur.clear();
    curw = 0;
  };
  for (auto& word : words) {
    int ww = 0;
    for (auto& c : word) ww += c.width;
    if (curw == 0) {
      cur = word;
      curw = ww;
      if (ww > width) {
        auto p = hard_break(word, width);
        lines.insert(lines.end(), p.begin(), p.end());
        cur.clear();
        curw = 0;
      }
      continue;
    }
    if (curw + 1 + ww <= width) {
      SC sp;
      sp.bytes = " ";
      sp.width = 1;
      sp.st = word.front().st;
      cur.push_back(sp);
      for (auto& c : word) cur.push_back(c);
      curw += 1 + ww;
    } else {
      flush();
      cur = word;
      curw = ww;
      if (ww > width) {
        auto p = hard_break(word, width);
        lines.insert(lines.end(), p.begin(), p.end());
        cur.clear();
        curw = 0;
      }
    }
  }
  flush();
  if (lines.empty()) lines.push_back("");
  return lines;
}

}  // namespace

int display_width(const std::string& s) {
  int w = 0;
  size_t i = 0;
  unsigned cp;
  size_t len;
  while ((len = utf8_decode(s, i, &cp)) > 0) {
    w += rune_width(cp);
    i += len;
  }
  return w;
}

std::string utf8_tail(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t start = s.size() - max_bytes;
  while (start < s.size() && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80)
    ++start;
  return s.substr(start);
}

std::string strip_ansi(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      // Consume intermediates (0x20-0x2F) and params (0x30-0x3F).
      while (j < s.size() && static_cast<unsigned char>(s[j]) >= 0x20 &&
             static_cast<unsigned char>(s[j]) <= 0x3F)
        ++j;
      // Consume the final byte (0x40-0x7E).
      if (j < s.size() && static_cast<unsigned char>(s[j]) >= 0x40 &&
          static_cast<unsigned char>(s[j]) <= 0x7E)
        ++j;
      i = j;
    } else {
      out += s[i];
      ++i;
    }
  }
  return out;
}

std::string fmt_int(long n) {
  bool neg = n < 0;
  std::string s = std::to_string(n < 0 ? -n : n);
  std::string r;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) {
    r.insert(0, 1, s[i]);
    if (++c % 3 == 0 && i > 0) r.insert(0, ",");
  }
  return (neg ? "-" : "") + r;
}

std::vector<std::string> markdown(const std::string& text, int width) {
  if (width <= 0) width = 80;
  auto lines = dog::util::split_lines(text);
  std::vector<std::string> out;
  bool in_code = false;
  std::string code_lang;
  std::vector<std::string> code_lines;

  auto flush_code = [&]() {
    if (!code_lang.empty())
      out.push_back("\x1b[2m\x1b[38;5;245m" + code_lang + "\x1b[0m");
    for (auto& ln : code_lines) {
      Style d;
      d.dim = true;
      std::vector<SC> chars;
      append_run(chars, ln.empty() ? " " : ln, d);
      auto wl = hard_break(chars, width - 2);
      for (auto& l : wl) out.push_back("  " + l);
    }
  };

  // Table state.
  bool in_table = false;
  std::string table_header;
  std::vector<std::string> table_data;

  auto split_cells = [](const std::string& row) -> std::vector<std::string> {
    std::vector<std::string> cells;
    std::string cur;
    for (char c : row) {
      if (c == '|') { cells.push_back(dog::util::trim(cur)); cur.clear(); }
      else cur += c;
    }
    cells.push_back(dog::util::trim(cur));
    if (!cells.empty() && cells.front().empty() && cells.size() > 1) cells.erase(cells.begin());
    if (!cells.empty() && cells.back().empty() && cells.size() > 1) cells.pop_back();
    return cells;
  };

  auto is_sep_row = [](const std::string& s) -> bool {
    if (s.empty()) return false;
    bool has_dash = false;
    for (char c : s) {
      if (c != '-' && c != '|' && c != ':' && c != ' ') return false;
      if (c == '-') has_dash = true;
    }
    return has_dash;
  };

  auto flush_table = [&]() {
    if (table_header.empty()) return;
    auto hdr = split_cells(table_header);
    int ncols = (int)hdr.size();
    for (auto& r : table_data) {
      auto c = split_cells(r);
      if ((int)c.size() > ncols) ncols = (int)c.size();
    }
    std::vector<int> cw(ncols, 0);
    for (int c = 0; c < ncols; c++) {
      if (c < (int)hdr.size()) cw[c] = std::max(cw[c], display_width(hdr[c]));
      for (auto& r : table_data) {
        auto cells = split_cells(r);
        if (c < (int)cells.size()) cw[c] = std::max(cw[c], display_width(cells[c]));
      }
    }
    auto make_sep = [&](const char* l, const char* m, const char* r) -> std::string {
      std::string s(l);
      for (int c = 0; c < ncols; c++) {
        for (int i = 0; i < cw[c] + 2; i++) s += "\xe2\x94\x80";
        s += (c < ncols - 1) ? m : r;
      }
      return s;
    };
    auto render_row = [&](const std::vector<std::string>& cells, bool is_header) -> std::string {
      Style base;
      base.bold = is_header;
      std::string s;
      for (int c = 0; c < ncols; c++) {
        s += "\xe2\x94\x82 ";
        int content_w = 0;
        if (c < (int)cells.size()) {
          auto rendered = render_sc(parse_inline(cells[c], base));
          content_w = display_width(strip_ansi(rendered));
          s += rendered;
        }
        for (int i = content_w; i < cw[c]; i++) s += ' ';
        s += " ";
      }
      s += "\xe2\x94\x82";
      return s;
    };
    out.push_back(make_sep("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90"));
    out.push_back(render_row(hdr, true));
    out.push_back(make_sep("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4"));
    for (size_t ri = 0; ri < table_data.size(); ri++) {
      out.push_back(render_row(split_cells(table_data[ri]), false));
      if (ri + 1 < table_data.size())
        out.push_back(make_sep("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4"));
      else
        out.push_back(make_sep("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98"));
    }
    out.push_back("");
    table_header.clear();
    table_data.clear();
    in_table = false;
  };

  for (size_t idx = 0; idx < lines.size(); idx++) {
    std::string line = lines[idx];
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (in_code) {
      std::string t = dog::util::trim(line);
      if (t == "```" || t == "~~~") {
        in_code = false;
        flush_code();
        code_lines.clear();
        code_lang.clear();
        continue;
      }
      code_lines.push_back(line);
      continue;
    }

    // Table continuation.
    if (in_table) {
      std::string tt = dog::util::trim(line);
      if (tt.empty()) continue;  // skip blank lines within table
      if (tt.find('|') != std::string::npos) { table_data.push_back(line); continue; }
      flush_table();
      // fall through to normal processing of this line
    }

    // Table start: current line has '|', next line is a separator.
    if (!in_table && line.find('|') != std::string::npos && idx + 1 < lines.size()) {
      std::string next = lines[idx + 1];
      if (!next.empty() && next.back() == '\r') next.pop_back();
      std::string nt = dog::util::trim(next);
      if (is_sep_row(nt) && nt.find('|') != std::string::npos) {
        in_table = true;
        table_header = line;
        table_data.clear();
        idx++;  // skip the separator line
        continue;
      }
    }

    std::string t = line;
    std::string ttrim = dog::util::trim(t);
    if (ttrim.empty()) {
      out.push_back("");
      continue;
    }

    // Fence opener.
    std::string lt = t;
    while (!lt.empty() && lt.front() == ' ') lt.erase(0, 1);
    if (lt.rfind("```", 0) == 0 || lt.rfind("~~~", 0) == 0) {
      in_code = true;
      code_lang = dog::util::trim(lt.substr(3));
      code_lines.clear();
      continue;
    }

    // Heading.
    size_t h = 0;
    while (h < t.size() && t[h] == '#') h++;
    if (h >= 1 && h <= 6 && (h == t.size() || t[h] == ' ')) {
      std::string content = t.substr(h);
      while (!content.empty() && content.back() == ' ') content.pop_back();
      while (!content.empty() && content.back() == '#') content.pop_back();
      while (!content.empty() && content.back() == ' ') content.pop_back();
      size_t sp = 0;
      while (sp < content.size() && content[sp] == ' ') sp++;
      content = content.substr(sp);
      Style hs;
      hs.bold = true;
      hs.color = 214;
      auto chars = parse_inline(content, hs);
      for (auto& l : wrap(chars, width)) out.push_back(l);
      out.push_back("");
      continue;
    }

    // Horizontal rule.
    {
      bool is_hr = ttrim.size() >= 3;
      if (is_hr) {
        char c0 = ttrim[0];
        if (c0 != '-' && c0 != '*' && c0 != '_') is_hr = false;
        else
          for (char x : ttrim)
            if (x != c0) is_hr = false;
      }
      if (is_hr) {
        std::string bar;
        int n = width < 200 ? width : 200;
        for (int i = 0; i < n; i++) bar += "\xe2\x94\x80";
        out.push_back("\x1b[2m\x1b[38;5;240m" + bar + "\x1b[0m");
        continue;
      }
    }

    // Blockquote.
    if (!t.empty() && t[0] == '>') {
      std::string content = t.substr(1);
      while (!content.empty() && content.front() == ' ') content.erase(0, 1);
      Style qs;
      qs.dim = true;
      auto chars = parse_inline(content, qs);
      // The "| " prefix is 2 columns; wrap content to the remaining width so the
      // final line never exceeds the terminal width (else it hard-wraps mid-word).
      auto wl = wrap(chars, width - 2);
      for (auto& l : wl) out.push_back("\x1b[2m\x1b[38;5;245m|\x1b[0m " + l);
      continue;
    }

    // List item.
    {
      size_t ind = 0;
      while (ind < t.size() && t[ind] == ' ') ind++;
      std::string rest = t.substr(ind);
      std::string bullet;
      int bullet_w = 0;
      bool is_list = false;
      if (rest.rfind("- ", 0) == 0 || rest.rfind("* ", 0) == 0 || rest.rfind("+ ", 0) == 0) {
        bullet = "\x1b[2m\x1b[38;5;245m\xe2\x97\x88\x1b[0m ";
        bullet_w = 2;
        is_list = true;
        rest = rest.substr(2);
      } else {
        size_t k = 0;
        while (k < rest.size() && std::isdigit(static_cast<unsigned char>(rest[k]))) k++;
        if (k >= 1 && k < rest.size() && (rest[k] == '.' || rest[k] == ')') &&
            k + 1 < rest.size() && rest[k + 1] == ' ') {
          bullet = "\x1b[2m\x1b[1m" + rest.substr(0, k) +
                   (rest[k] == '.' ? "." : ")") + "\x1b[0m ";
          bullet_w = (int)k + 2;
          is_list = true;
          rest = rest.substr(k + 1);
        }
      }
      if (is_list) {
        auto chars = parse_inline(rest, Style());
        // The bullet / "N. " prefix occupies bullet_w columns; wrap the content to
        // the remaining width so the line never exceeds the terminal width.
        auto wl = wrap(chars, width - bullet_w);
        std::string pad(bullet_w, ' ');
        for (size_t i = 0; i < wl.size(); i++)
          out.push_back(i == 0 ? bullet + wl[i] : pad + wl[i]);
        continue;
      }
    }

    // Normal paragraph line.
    auto chars = parse_inline(line, Style());
    for (auto& l : wrap(chars, width)) out.push_back(l);
  }

  if (in_code) flush_code();
  if (in_table) flush_table();

  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

}  // namespace dog::render
