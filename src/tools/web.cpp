#include "tools/web.hpp"
#include "llm/http.hpp"
#include "util.hpp"
#include <map>
#include <string>

namespace dog {

namespace {

const size_t kDefaultMaxChars = 30000;

bool is_http_url(const std::string& u) {
  return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

std::string do_web_fetch(const nlohmann::json& args) {
  std::string url = args.value("url", "");
  if (url.empty()) return "error: url is required";
  if (!is_http_url(url)) return "error: url must start with http:// or https://";
  size_t max_chars = kDefaultMaxChars;
  if (args.contains("max_chars")) {
    long v = args["max_chars"].get<long>();
    if (v > 0) max_chars = (size_t)v;
  }

  std::map<std::string, std::string> hdr = {{"User-Agent", "dog/0.1"}};
  std::string body;
  std::vector<std::string> raw_hdr;
  std::string err;
  int status = http_get_ex(url, hdr, &body, &raw_hdr, &err);
  if (status == 0) return "error: " + err;

  // If the server declares an image content type (or the magic bytes match),
  // return the image as a tagged base64 payload so the model can see it.
  std::string ctype;
  for (auto& h : raw_hdr) {
    std::string low;
    low.reserve(h.size());
    for (char c : h) low.push_back((char)std::tolower((unsigned char)c));
    if (low.rfind("content-type:", 0) == 0)
      ctype = util::trim(h.substr(std::string("content-type:").size()));
  }
  std::string mime = util::sniff_image_mime(body);
  if (mime.empty())
    for (auto& t : util::split_on(ctype, ';'))
      if (util::trim(t).rfind("image/", 0) == 0) {
        mime = util::trim(t);
        break;
      }
  if (!mime.empty())
    return std::string("[image:") + mime + "]" + util::base64_encode(body);

  std::string out = "HTTP " + std::to_string(status) + "\n";
  if (body.size() > max_chars)
    out += util::truncate(body, max_chars);
  else
    out += util::sanitize_binary(body);
  return out;
}

}  // namespace

void register_web_tools(ToolRegistry& r) {
  r.add("web_fetch",
        "Fetch the body of an http:// or https:// URL via a single GET request. Returns "
        "'HTTP <status>' followed by the response body, truncated to max_chars.",
        {{"type", "object"},
         {"properties",
          {{"url", {{"type", "string"}, {"description", "Absolute http(s) URL"}}},
           {"max_chars", {{"type", "integer"},
                          {"description", "Max body characters to return (default 30000)"}}}}},
         {"required", {"url"}},
         {"additionalProperties", false}},
        /*read_only=*/true, do_web_fetch);
}

}  // namespace dog
