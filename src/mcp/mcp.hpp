#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "config/config.hpp"
#include "tools/registry.hpp"

namespace dog {

// One tool advertised by an MCP server (from tools/list).
struct McpTool {
  std::string name;
  std::string description;
  nlohmann::json input_schema;
  bool read_only = false;  // from annotations.readOnlyHint
};

// A JSON-RPC 2.0 MCP client bound to one transport (stdio or streamable HTTP).
// Not copyable: it owns a live connection.
class McpClient {
public:
  explicit McpClient(McpServerConfig cfg) : cfg_(std::move(cfg)) {}
  virtual ~McpClient();

  // Transport setup + initialize/initialized handshake. Sets *err on failure.
  bool connect(std::string* err);
  std::vector<McpTool> list_tools(std::string* err);
  // Returns the tool's text output; sets *is_error from result.isError.
  std::string call_tool(const std::string& name, const nlohmann::json& args,
                        bool* is_error, std::string* err);

protected:
  // Send a request (has id), return the matching response (empty on transport
  // failure, which also sets *err). Protocol-level errors are in resp["error"].
  nlohmann::json request(const std::string& method, const nlohmann::json& params,
                         std::string* err);
  void notify(const std::string& method, const nlohmann::json& params, std::string* err);

  // Transport round-trip. If msg has an "id", block until the matching response
  // and return it; otherwise send and return an empty json.
  virtual nlohmann::json send(const nlohmann::json& msg, std::string* err) = 0;
  virtual bool transport_init(std::string* err) = 0;
  virtual void transport_cleanup() {}

  McpServerConfig cfg_;
  int next_id_ = 1;
};

// stdio transport: spawns cfg_.command and speaks newline-delimited JSON-RPC
// over its stdin/stdout.
class McpStdioClient : public McpClient {
public:
  explicit McpStdioClient(McpServerConfig cfg) : McpClient(std::move(cfg)) {}

private:
  nlohmann::json send(const nlohmann::json& msg, std::string* err) override;
  bool transport_init(std::string* err) override;
  void transport_cleanup() override;

  bool spawn(std::string& err);
  bool read_line(std::string& line, long timeout_ms);
  bool write_line(const std::string& line);

  pid_t child_pid_ = -1;
  int stdin_fd_ = -1;   // our write end -> child stdin
  int stdout_fd_ = -1;  // our read end <- child stdout
  std::string buf_;
};

// streamable-HTTP transport: POST JSON-RPC to cfg_.url; reply is plain JSON or
// an SSE stream; tracks the mcp-session-id response header.
class McpHttpClient : public McpClient {
public:
  explicit McpHttpClient(McpServerConfig cfg) : McpClient(std::move(cfg)) {}

private:
  nlohmann::json send(const nlohmann::json& msg, std::string* err) override;
  bool transport_init(std::string* err) override;

  std::string session_id_;
};

McpClient* make_mcp_client(const McpServerConfig& cfg);

// Connects to every configured server, discovers tools, and registers them
// into `reg` as `mcp__<server>__<tool>`. Owns the clients for its lifetime.
class McpRegistry {
public:
  void start(const std::vector<McpServerConfig>& servers, ToolRegistry& reg,
             std::function<void(const std::string&)> log = {});

private:
  std::vector<std::unique_ptr<McpClient>> clients_;
};

}  // namespace dog
