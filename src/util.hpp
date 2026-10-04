#pragma once
#include <algorithm>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Global cooperative-interrupt flag. Set by the UI (Ctrl-C while a turn is
// running) and checked at checkpoints (model streaming, shell, tools) so an
// in-flight turn can be aborted cleanly and the prompt returned to the user.
namespace dog {
inline std::atomic<bool> interrupt_flag{false};
}  // namespace dog

namespace dog::util {
inline bool interrupted() { return interrupt_flag.load(); }
inline void request_interrupt() { interrupt_flag.store(true); }

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline std::string truncate(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "...";
}

inline bool is_dir(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else
      cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

inline std::vector<std::string> split_on(const std::string& s, char d) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == d) {
      out.push_back(cur);
      cur.clear();
    } else
      cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

// Strict UTF-8 validation. A binary blob (e.g. a PNG from web_fetch/read_file) is
// invalid: 0x89 is not a legal UTF-8 lead byte.
inline bool is_valid_utf8(const std::string& s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    } else {
      return false;  // stray continuation byte or 0xF8-0xFF
    }
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      unsigned char t = static_cast<unsigned char>(s[i + k]);
      if ((t >> 6) != 2) return false;  // must be 10xxxxxx
    }
    i += len;
  }
  return true;
}

// Guard before a tool result or user input is pushed into the message history.
// nlohmann throws type_error.316 on dump() if any stored string is invalid UTF-8,
// which crashes the whole harness when a tool returns binary (images, etc.).
// Binary content is replaced by a short valid-ASCII placeholder instead.
inline std::string sanitize_binary(const std::string& s) {
  if (is_valid_utf8(s)) return s;
  return "[binary data omitted: " + std::to_string(s.size()) +
         " bytes, not valid UTF-8]";
}

// Standard base64 (no line wrapping). Used to embed images as data: URIs so
// the wire payload stays valid UTF-8 and OpenAI accepts it.
inline std::string base64_encode(const std::string& in) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                 (static_cast<unsigned char>(in[i + 1]) << 8) |
                 static_cast<unsigned char>(in[i + 2]);
    out += tbl[v >> 18];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    unsigned v = static_cast<unsigned char>(in[i]) << 4;
    out += tbl[v >> 6];
    out += tbl[v & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 8) |
                 static_cast<unsigned char>(in[i + 1]);
    out += tbl[(v >> 10) & 63];
    out += tbl[(v >> 4) & 63];
    out += tbl[(v & 15) << 2];
    out += '=';
  }
  return out;
}

// Image magic-byte signatures. Returns the MIME type when the leading bytes
// match a known image, else "".
inline std::string sniff_image_mime(const std::string& b) {
  auto eq = [&](size_t pos, const char* sig, size_t len) {
    if (b.size() < pos + len) return false;
    for (size_t i = 0; i < len; i++)
      if ((unsigned char)b[pos + i] != (unsigned char)sig[i]) return false;
    return true;
  };
  if (b.size() >= 8 && (unsigned char)b[0] == 0x89 && eq(1, "PNG", 3))
    return "image/png";
  if (b.size() >= 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8 &&
      (unsigned char)b[2] == 0xFF)
    return "image/jpeg";
  if (b.size() >= 6 && eq(0, "GIF87a", 6)) return "image/gif";
  if (b.size() >= 6 && eq(0, "GIF89a", 6)) return "image/gif";
  if (b.size() >= 12 && eq(0, "RIFF", 4) && eq(8, "WEBP", 4)) return "image/webp";
  if (b.size() >= 6 && eq(0, "BM", 2)) return "image/bmp";
  return "";
}

// Extension -> MIME fallback for sniff_image_mime misses.
inline std::string mime_from_path(const std::string& path) {
  std::string low;
  for (char c : path) low.push_back((char)std::tolower((unsigned char)c));
  if (low.size() >= 4 && low.substr(low.size() - 4) == ".png") return "image/png";
  if (low.size() >= 5 && low.substr(low.size() - 5) == ".jpeg") return "image/jpeg";
  if (low.size() >= 4 && low.substr(low.size() - 4) == ".jpg") return "image/jpeg";
  if (low.size() >= 4 && low.substr(low.size() - 4) == ".gif") return "image/gif";
  if (low.size() >= 5 && low.substr(low.size() - 5) == ".webp") return "image/webp";
  if (low.size() >= 4 && low.substr(low.size() - 4) == ".bmp") return "image/bmp";
  return "";
}

// Expand a leading "~" or "~/..." to $HOME. User-friendly paths like
// "~/images.jpeg" must resolve before the file is opened (else the read fails
// but the extension check still flags it as an image, producing an empty part).
inline std::string expand_home(const std::string& path) {
  if (!path.empty() && path[0] == '~') {
    const char* home = std::getenv("HOME");
    std::string h = (home && *home) ? std::string(home) : std::string();
    if (h.empty()) return path;  // no HOME: leave as-is, let open() fail
    if (path.size() > 1 && path[1] == '/') return h + path.substr(1);  // ~/...
    return h + path;              // "~file" -> "$HOMEfile"
  }
  return path;
}

// True when the file at `path` is an image (by magic bytes, falling back to its
// extension). Used by read_file to decide whether to inline it as a vision part.
inline bool is_image_file(const std::string& path) {
  std::string head;
  {
    std::ifstream f(path, std::ios::binary);
    char buf[16] = {0};
    f.read(buf, sizeof buf);
    head.assign(buf, f.gcount());
  }
  if (!sniff_image_mime(head).empty()) return true;
  return !mime_from_path(path).empty();
}

// Create all missing parent directories for `path` (not the final component).
inline bool ensure_parent_dir(const std::string& path) {
  std::string p = path;
  size_t slash = 0;
  while (true) {
    slash = p.find('/', slash);
    if (slash == std::string::npos) break;
    std::string d = p.substr(0, slash);
    if (!d.empty() && d != "/") {
      struct stat st;
      if (::stat(d.c_str(), &st) != 0) {
        if (::mkdir(d.c_str(), 0755) != 0) return false;
      }
    }
    ++slash;
  }
  return true;
}

}  // namespace dog::util
