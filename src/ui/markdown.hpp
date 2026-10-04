#pragma once
#include <string>
#include <vector>

namespace dog::render {

// Render a block of markdown as word-wrapped terminal lines (with ANSI styling).
// `width` is the target display width in columns (<= 0 -> 80).
std::vector<std::string> markdown(const std::string& text, int width);

// --- small text utilities shared by the UI ---

// Terminal display width of s (ANSI ignored; wide CJK counted as 2).
int display_width(const std::string& s);
// The last `max_bytes` of s, backed up to a UTF-8 rune boundary.
std::string utf8_tail(const std::string& s, size_t max_bytes);
// Remove ANSI escape sequences.
std::string strip_ansi(const std::string& s);
// Format an integer with thousands separators (12345 -> "12,345").
std::string fmt_int(long n);

}  // namespace dog::render
