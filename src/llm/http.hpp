#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dog {

// Streaming HTTP POST. on_chunk is invoked with raw response-body bytes as they
// arrive (used for SSE). Returns the HTTP status code, or 0 on transport error.
int http_post(const std::string& url,
              const std::map<std::string, std::string>& headers,
              const std::string& body,
              std::function<void(const char*, size_t)> on_chunk,
              std::string* err);

// Non-streaming HTTP GET. Fills body with the full response. Returns status or 0.
int http_get(const std::string& url,
             const std::map<std::string, std::string>& headers,
             std::string* body,
             std::string* err);

// HTTP POST that also captures the raw response header lines (e.g. to read
// "mcp-session-id"). Fills body_out with the full response. Returns status or 0.
int http_post_ex(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 const std::string& body,
                 std::string* body_out,
                 std::vector<std::string>* header_lines,
                 std::string* err);

// HTTP GET that also captures the raw response header lines (e.g. Content-Type),
// so the caller can sniff a binary body by its declared MIME. Fills body_out with
// the full response. Returns status or 0.
int http_get_ex(const std::string& url,
                const std::map<std::string, std::string>& headers,
                std::string* body_out,
                std::vector<std::string>* header_lines,
                std::string* err);

}  // namespace dog
