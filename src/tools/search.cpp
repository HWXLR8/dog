#include "tools/search.hpp"
#include "util.hpp"
#include <algorithm>
#include <dirent.h>
#include <fnmatch.h>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <sys/stat.h>

namespace dog {

namespace {

struct Entry {
  std::string name;
  bool is_dir;
};

std::vector<Entry> list_dir(const std::string& dir) {
  std::vector<Entry> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st;
    bool isd = false;
    std::string full = dir + "/" + name;
    if (::stat(full.c_str(), &st) == 0)
      isd = S_ISDIR(st.st_mode);
    out.push_back({name, isd});
  }
  ::closedir(d);
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  return out;
}

bool seg_match(const std::string& name, const std::string& pat) {
  return ::fnmatch(pat.c_str(), name.c_str(), 0) == 0;
}

void glob_rec(const std::string& dir, const std::vector<std::string>& segs, size_t i,
              std::vector<std::string>& out) {
  if (i == segs.size()) {
    out.push_back(dir);
    return;
  }
  const std::string& seg = segs[i];
  std::vector<Entry> entries = list_dir(dir);
  if (seg == "**") {
    glob_rec(dir, segs, i + 1, out);  // zero levels
    for (auto& e : entries)
      if (e.is_dir) glob_rec(dir + "/" + e.name, segs, i, out);  // one or more levels
    return;
  }
  for (auto& e : entries)
    if (seg_match(e.name, seg)) glob_rec(dir + "/" + e.name, segs, i + 1, out);
}

std::vector<std::string> collect_files(const std::string& root) {
  std::vector<std::string> out;
  std::vector<std::string> stack = {root};
  while (!stack.empty()) {
    std::string cur = stack.back();
    stack.pop_back();
    if (util::is_dir(cur)) {
      for (auto& e : list_dir(cur)) {
        std::string full = cur + "/" + e.name;
        if (e.is_dir) {
          if (e.name == ".git" || e.name == "node_modules") continue;
          stack.push_back(full);
        } else {
          out.push_back(full);
        }
      }
    } else {
      out.push_back(cur);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string do_glob(const nlohmann::json& args) {
  std::string pattern = args.value("pattern", "");
  std::string base = args.value("path", std::string("."));
  if (pattern.empty()) return "error: pattern is required";
  std::string start_dir = base;
  std::string pat = pattern;
  if (!pat.empty() && pat[0] == '/') {
    start_dir = "/";
    pat = pat.substr(1);
  }
  std::vector<std::string> segs;
  for (auto& s : util::split_on(pat, '/'))
    if (!s.empty()) segs.push_back(s);
  std::vector<std::string> out;
  glob_rec(start_dir, segs, 0, out);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (out.empty()) return "no matches";
  std::string r;
  for (auto& f : out) r += f + "\n";
  return r;
}

std::string do_grep(const nlohmann::json& args) {
  std::string pattern = args.value("pattern", "");
  std::string path = args.value("path", std::string("."));
  std::string glob_filter;
  if (args.contains("glob")) glob_filter = args["glob"].get<std::string>();
  int limit = 100;
  if (args.contains("limit")) limit = args["limit"].get<int>();
  if (pattern.empty()) return "error: pattern is required";

  bool use_regex = true;
  std::regex re;
  std::string literal = pattern;
  try {
    re = std::regex(pattern);
  } catch (...) {
    use_regex = false;
  }

  std::vector<std::string> files = collect_files(path);
  if (!glob_filter.empty()) {
    std::vector<std::string> kept;
    for (auto& f : files) {
      std::string base = f;
      size_t slash = f.find_last_of('/');
      if (slash != std::string::npos) base = f.substr(slash + 1);
      bool ok;
      if (glob_filter.find('/') == std::string::npos)
        ok = seg_match(base, glob_filter);
      else
        ok = seg_match(f, glob_filter);
      if (ok) kept.push_back(f);
    }
    files = kept;
  }

  std::string out;
  int count = 0;
  for (auto& f : files) {
    if (count >= limit) break;
    std::string content;
    {
      std::ifstream in(f, std::ios::binary);
      if (!in.good()) continue;
      std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (data.empty() || data.size() > 1000000) continue;
      if (data.find('\0') != std::string::npos) continue;  // skip binary
      content = std::move(data);
    }
    std::vector<std::string> lines = util::split_lines(content);
    for (size_t i = 0; i < lines.size(); ++i) {
      if (count >= limit) break;
      bool matched = use_regex ? std::regex_search(lines[i], re)
                               : (lines[i].find(literal) != std::string::npos);
      if (matched) {
        out += f + ":" + std::to_string(i + 1) + ":" + util::trim(lines[i]) + "\n";
        ++count;
      }
    }
  }
  if (count >= limit) out += "... (truncated at " + std::to_string(limit) + " matches)\n";
  if (out.empty()) return "no matches";
  return out;
}

}  // namespace

void register_search_tools(ToolRegistry& r) {
  r.add("glob",
        "Find files by glob pattern. Supports * ? [..] and ** (any number of path "
        "segments). Relative patterns match against the path argument (default cwd).",
        {{"type", "object"},
         {"properties",
          {{"pattern", {{"type", "string"}, {"description", "Glob pattern"}}},
           {"path", {{"type", "string"}, {"description", "Base directory (default .)"}}}}},
         {"required", {"pattern"}},
         {"additionalProperties", false}},
        /*read_only=*/true, do_glob);

  r.add("grep",
        "Search file contents for a regex pattern. Recurses directories (skips .git and "
        "node_modules). Returns path:line:text matches. Optionally filter by a glob.",
        {{"type", "object"},
         {"properties",
          {{"pattern", {{"type", "string"}, {"description", "Regex pattern"}}},
           {"path", {{"type", "string"}, {"description", "File or directory (default .)"}}},
           {"glob", {{"type", "string"}, {"description", "Optional file glob filter"}}},
           {"limit", {{"type", "integer"}, {"description", "Max matches (default 100)"}}}}},
         {"required", {"pattern"}},
         {"additionalProperties", false}},
        /*read_only=*/true, do_grep);
}

}  // namespace dog
