#include "mcp/mcp.hpp"
#include "llm/http.hpp"
#include "util.hpp"
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dog {

// ---------- base (transport-agnostic JSON-RPC) ----------

McpClient::~McpClient() { transport_cleanup(); }

bool McpClient::connect(std::string* err) {
  if (!transport_init(err)) return false;
  nlohmann::json params = {
      {"protocolVersion", "2024-11-05"},
      {"capabilities", nlohmann::json::object()},
      {"clientInfo", {{"name", "dog"}, {"version", "0.1"}}},
  };
  nlohmann::json resp = request("initialize", params, err);
  if (resp.empty()) return false;
  if (resp.contains("error")) {
    if (err) *err = "initialize failed: " + resp["error"].value("message", "unknown");
    return false;
  }
  notify("notifications/initialized", nlohmann::json::object(), err);
  return true;
}

nlohmann::json McpClient::request(const std::string& method, const nlohmann::json& params,
                                  std::string* err) {
  nlohmann::json msg = {{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}};
  if (!params.is_null()) msg["params"] = params;
  return send(msg, err);
}

void McpClient::notify(const std::string& method, const nlohmann::json& params,
                       std::string* err) {
  nlohmann::json msg = {{"jsonrpc", "2.0"}, {"method", method}};
  if (!params.is_null() && !params.empty()) msg["params"] = params;
  send(msg, err);
}

std::vector<McpTool> McpClient::list_tools(std::string* err) {
  std::vector<McpTool> tools;
  nlohmann::json resp = request("tools/list", nlohmann::json::object(), err);
  if (resp.empty()) return tools;
  if (resp.contains("error")) {
    if (err) *err = resp["error"].value("message", "unknown");
    return tools;
  }
  nlohmann::json result = resp.value("result", nlohmann::json::object());
  if (!result.is_object()) return tools;
  nlohmann::json arr = result.value("tools", nlohmann::json::array());
  for (auto& t : arr) {
    McpTool mt;
    mt.name = t.value("name", "");
    mt.description = t.value("description", "");
    mt.input_schema = t.value("inputSchema", nlohmann::json::object());
    if (t.contains("annotations") && t["annotations"].contains("readOnlyHint") &&
        t["annotations"]["readOnlyHint"].is_boolean())
      mt.read_only = t["annotations"]["readOnlyHint"].get<bool>();
    if (!mt.name.empty()) tools.push_back(std::move(mt));
  }
  return tools;
}

std::string McpClient::call_tool(const std::string& name, const nlohmann::json& args,
                                 bool* is_error, std::string* err) {
  if (is_error) *is_error = false;
  nlohmann::json params = {
      {"name", name},
      {"arguments", (args.is_null() ? nlohmann::json::object() : args)},
  };
  nlohmann::json resp = request("tools/call", params, err);
  if (resp.empty()) {
    std::string e = err ? *err : "";
    return e.empty() ? "error: no response from MCP server" : ("error: " + e);
  }
  if (resp.contains("error")) return "error: " + resp["error"].value("message", "unknown");
  nlohmann::json result = resp.value("result", nlohmann::json::object());
  bool is_err = false;
  if (result.is_object() && result.contains("isError")) is_err = result["isError"].get<bool>();
  std::string out;
  nlohmann::json content = result.value("content", nlohmann::json::array());
  if (content.is_array())
    for (auto& c : content)
      if (c.value("type", "") == "text") {
        if (!out.empty()) out += "\n";
        out += c.value("text", "");
      }
  if (out.empty()) out = "(no output)";
  if (is_error) *is_error = is_err;
  if (is_err) out = "error: " + out;
  return out;
}

// ---------- stdio transport ----------

bool McpStdioClient::transport_init(std::string* err) { return spawn(*err); }

void McpStdioClient::transport_cleanup() {
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
  if (stdout_fd_ >= 0) {
    ::close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (child_pid_ > 0) {
    ::kill(child_pid_, SIGHUP);
    ::waitpid(child_pid_, nullptr, 0);
    child_pid_ = -1;
  }
}

bool McpStdioClient::spawn(std::string& err) {
  if (cfg_.command.empty()) {
    err = "stdio server has no command";
    return false;
  }
  int in_pipe[2], out_pipe[2];
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
    err = "pipe() failed";
    return false;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    err = "fork() failed";
    return false;
  }
  if (pid == 0) {
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      ::dup2(dn, STDERR_FILENO);
      ::close(dn);
    }
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(cfg_.args.size() + 2);
    argv.push_back(const_cast<char*>(cfg_.command.c_str()));
    for (auto& a : cfg_.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execvp(cfg_.command.c_str(), argv.data());
    ::_exit(127);  // exec failed
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  stdin_fd_ = in_pipe[1];
  stdout_fd_ = out_pipe[0];
  child_pid_ = pid;
  return true;
}

bool McpStdioClient::write_line(const std::string& line) {
  std::string s = line + "\n";
  size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::write(stdin_fd_, s.data() + off, s.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += (size_t)w;
  }
  return true;
}

bool McpStdioClient::read_line(std::string& line, long timeout_ms) {
  line.clear();
  for (;;) {
    size_t nl = buf_.find('\n');
    if (nl != std::string::npos) {
      line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      return true;
    }
    struct pollfd pfd;
    pfd.fd = stdout_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = ::poll(&pfd, 1, (int)timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0) return false;  // timeout
    char tmp[8192];
    ssize_t n = ::read(stdout_fd_, tmp, sizeof(tmp));
    if (n > 0) {
      buf_.append(tmp, (size_t)n);
    } else if (n == 0) {
      if (!buf_.empty()) {
        line = std::move(buf_);
        buf_.clear();
        return true;
      }
      return false;  // EOF
    } else {
      if (errno == EINTR) continue;
      return false;
    }
  }
}

nlohmann::json McpStdioClient::send(const nlohmann::json& msg, std::string* err) {
  if (!write_line(msg.dump())) {
    if (err) *err = "write to MCP stdin failed";
    return {};
  }
  if (!msg.contains("id")) return nlohmann::json();  // notification: no reply
  int want = msg["id"].get<int>();
  for (int i = 0; i < 10000; ++i) {
    std::string rl;
    if (!read_line(rl, 15000)) {
      if (err) *err = "timeout reading MCP response";
      return {};
    }
    if (rl.empty()) continue;
    nlohmann::json m;
    try {
      m = nlohmann::json::parse(rl);
    } catch (...) {
      continue;  // stray/log line, not JSON
    }
    if (m.contains("id") && m["id"].is_number_integer() && m["id"].get<int>() == want) return m;
  }
  if (err) *err = "no matching MCP response";
  return {};
}

// ---------- streamable-HTTP transport ----------

namespace {

std::vector<nlohmann::json> parse_mcp_body(const std::string& body) {
  std::vector<nlohmann::json> out;
  bool sse = false;
  for (auto& raw : util::split_lines(body)) {
    std::string t = util::trim(raw);
    if (t.rfind("data:", 0) == 0) {
      std::string payload = util::trim(t.substr(5));
      if (payload.empty() || payload == "[DONE]") continue;
      try {
        out.push_back(nlohmann::json::parse(payload));
      } catch (...) {
      }
      sse = true;
    }
  }
  if (sse) return out;
  try {
    nlohmann::json j = nlohmann::json::parse(body);
    if (j.is_array())
      for (auto& x : j) out.push_back(x);
    else if (!j.is_null()) out.push_back(j);
  } catch (...) {
  }
  return out;
}

std::string sanitize_server_name(const std::string& s) {
  std::string r;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    r.push_back(ok ? c : '_');
  }
  if (r.empty() || !std::isalpha((unsigned char)r[0])) r = "s" + r;
  return r;
}

}  // namespace

bool McpHttpClient::transport_init(std::string* /*err*/) { return true; }

nlohmann::json McpHttpClient::send(const nlohmann::json& msg, std::string* err) {
  std::map<std::string, std::string> hdr = {
      {"Content-Type", "application/json"},
      {"Accept", "application/json, text/event-stream"},
  };
  if (!session_id_.empty()) hdr["mcp-session-id"] = session_id_;
  std::string body;
  std::vector<std::string> rh;
  std::string e;
  int status = http_post_ex(cfg_.url, hdr, msg.dump(), &body, &rh, &e);
  if (status == 0) {
    if (err) *err = e;
    return {};
  }
  for (auto& h : rh) {
    std::string low;
    low.reserve(h.size());
    for (char c : h) low.push_back((char)std::tolower((unsigned char)c));
    if (low.rfind("mcp-session-id:", 0) == 0)
      session_id_ = util::trim(h.substr(std::string("mcp-session-id:").size()));
  }
  if (status == 202) return nlohmann::json();  // accepted, no body (notification)
  if (status != 200) {
    if (err) *err = "HTTP " + std::to_string(status) + " from MCP server";
    return {};
  }
  std::vector<nlohmann::json> objs = parse_mcp_body(body);
  if (msg.contains("id"))
    for (auto& o : objs)
      if (o.contains("id") && o["id"] == msg["id"]) return o;
  if (!objs.empty()) return objs.back();
  if (err) *err = "empty MCP HTTP response";
  return {};
}

// ---------- factory + registry ----------

McpClient* make_mcp_client(const McpServerConfig& cfg) {
  if (cfg.transport == "http" || cfg.transport == "sse" || cfg.transport == "streamable-http")
    return new McpHttpClient(cfg);
  return new McpStdioClient(cfg);
}

void McpRegistry::start(const std::vector<McpServerConfig>& servers, ToolRegistry& reg,
                        std::function<void(const std::string&)> log) {
  for (const auto& scfg : servers) {
    if (scfg.name.empty()) continue;
    std::string err;
    std::unique_ptr<McpClient> client(make_mcp_client(scfg));
    if (!client->connect(&err)) {
      if (log) log("MCP " + scfg.name + " failed to connect: " + err);
      continue;
    }
    std::vector<McpTool> tools = client->list_tools(&err);
    if (!err.empty()) {
      if (log) log("MCP " + scfg.name + " tools/list failed: " + err);
      continue;
    }
    std::string prefix = "mcp__" + sanitize_server_name(scfg.name) + "__";
    McpClient* raw = client.get();
    for (const auto& t : tools) {
      nlohmann::json schema = t.input_schema;
      if (!schema.is_object()) schema = nlohmann::json::object();
      if (!schema.contains("type")) schema["type"] = "object";
      if (!schema.contains("properties")) schema["properties"] = nlohmann::json::object();
      std::string full = prefix + t.name;
      std::string desc = t.description.empty() ? ("MCP tool " + full) : t.description;
      bool ro = t.read_only;
      std::string server_tool = t.name;
      reg.add(full, desc, schema, ro, [raw, server_tool](const nlohmann::json& args) {
        bool is_err = false;
        std::string e;
        return raw->call_tool(server_tool, args, &is_err, &e);
      });
    }
    if (log)
      log("MCP " + scfg.name + " connected (" + std::to_string(tools.size()) + " tools)");
    clients_.push_back(std::move(client));
  }
}

}  // namespace dog
