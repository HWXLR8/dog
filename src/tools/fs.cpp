#include "tools/fs.hpp"
#include "util.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace dog {

namespace {
std::string read_whole(const std::string& path, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) {
    *err = "cannot open file: " + path;
    return "";
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

const size_t kMaxRead = 120000;

std::string do_read_file(const nlohmann::json& args) {
  std::string path = args.value("path", "");
  if (path.empty()) return "error: path is required";
  std::string err;
  std::string content = read_whole(path, &err);
  if (!err.empty()) return err;

  // Images: return a tagged base64 payload so the model can see it. The agent
  // loop detects the [image:...] tag, splits it out of the (text-only) tool
  // message, and sends it as an OpenAI image_url part.
  if (util::is_image_file(path)) {
    std::string mime = util::sniff_image_mime(content);
    if (mime.empty()) mime = util::mime_from_path(path);
    if (mime.empty()) mime = "image/png";
    return std::string("[image:") + mime + "]" + util::base64_encode(content);
  }

  std::vector<std::string> lines = util::split_lines(content);
  size_t start = 0;
  size_t end = lines.size();
  if (args.contains("offset")) start = static_cast<size_t>(args["offset"].get<int>());
  if (args.contains("limit")) {
    size_t lim = static_cast<size_t>(args["limit"].get<int>());
    end = std::min(lines.size(), start + lim);
  }
  if (start >= lines.size()) start = 0;
  if (start > end) end = start;

  std::string result;
  for (size_t i = start; i < end; ++i) result += lines[i] + "\n";
  if (result.size() > kMaxRead)
    result = result.substr(0, kMaxRead) +
             "\n... [truncated; file is " + std::to_string(content.size()) + " bytes]";
  if (result.empty()) return "(empty file)";
  return result;
}

std::string do_write_file(const nlohmann::json& args) {
  std::string path = args.value("path", "");
  if (path.empty()) return "error: path is required";
  std::string content;
  if (args.contains("content")) content = args["content"].get<std::string>();
  if (!util::ensure_parent_dir(path)) return "error: cannot create parent dir for " + path;
  std::ofstream f(path, std::ios::binary);
  if (!f.good()) return "error: cannot open for write: " + path;
  f << content;
  return "OK: wrote " + std::to_string(content.size()) + " bytes to " + path;
}

std::string do_edit_file(const nlohmann::json& args) {
  std::string path = args.value("path", "");
  std::string old_s = args.value("old_string", "");
  std::string new_s;
  bool replace_all = args.value("replace_all", false);
  if (args.contains("new_string")) new_s = args["new_string"].get<std::string>();
  if (path.empty() || old_s.empty()) return "error: path and old_string are required";

  std::string err;
  std::string content = read_whole(path, &err);
  if (!err.empty()) return err;

  size_t pos = content.find(old_s);
  if (pos == std::string::npos) return "error: old_string not found in " + path;

  // Compute 1-based line number of the first occurrence.
  int line_no = 1;
  for (size_t i = 0; i < pos; i++)
    if (content[i] == '\n') line_no++;

  std::string result;
  size_t last = 0;
  size_t count = 0;
  while (pos != std::string::npos) {
    result += content.substr(last, pos - last);
    result += new_s;
    ++count;
    last = pos + old_s.size();
    if (!replace_all) break;
    pos = content.find(old_s, last);
  }
  result += content.substr(last);

  std::ofstream f(path, std::ios::binary);
  if (!f.good()) return "error: cannot open for write: " + path;
  f << result;
  return "OK: replaced " + std::to_string(count) + " occurrence(s) at line " +
         std::to_string(line_no) + " in " + path;
}

}  // namespace

void register_fs_tools(ToolRegistry& r) {
  r.add("read_file",
        "Read the contents of a text file. Returns up to ~120KB; larger files are "
        "truncated with a note. Optionally read a line range via offset/limit. Image "
        "files (png/jpg/webp/gif/bmp) return the image so you can view it.",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}, {"description", "Absolute path to the file"}}},
           {"offset",
            {{"type", "integer"}, {"description", "Optional 0-based line to start at"}}},
           {"limit",
            {{"type", "integer"}, {"description", "Optional max number of lines"}}}}},
         {"required", {"path"}},
         {"additionalProperties", false}},
        /*read_only=*/true, do_read_file);

  r.add("write_file",
        "Create or overwrite a file with the given content. Parent directories are "
        "created if missing.",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}, {"description", "Absolute path to write"}}},
           {"content", {{"type", "string"}, {"description", "Full file content"}}}}},
         {"required", {"path", "content"}},
         {"additionalProperties", false}},
        /*read_only=*/false, do_write_file);

  r.add("edit_file",
        "Replace an exact string in a file. Fails if old_string is not found. Use "
        "replace_all to replace every occurrence.",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}, {"description", "Absolute path to the file"}}},
           {"old_string", {{"type", "string"}, {"description", "Exact text to replace"}}},
           {"new_string", {{"type", "string"}, {"description", "Replacement text"}}},
           {"replace_all",
            {{"type", "boolean"}, {"description", "Replace all occurrences"}}}}},
         {"required", {"path", "old_string", "new_string"}},
         {"additionalProperties", false}},
        /*read_only=*/false, do_edit_file);
}

}  // namespace dog
