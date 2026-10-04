#include "tools/shell.hpp"
#include "util.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace dog {

namespace {

const size_t kMaxOutput = 20000;

std::string do_shell(const nlohmann::json& args) {
  std::string command = args.value("command", "");
  if (command.empty()) return "error: command is required";
  std::string cwd;
  if (args.contains("cwd")) cwd = args["cwd"].get<std::string>();
  int timeout_s = 120;
  if (args.contains("timeout_ms")) timeout_s = std::max(1, args["timeout_ms"].get<int>() / 1000);

  int out_pipe[2], err_pipe[2];
  if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) return "error: pipe() failed";

  pid_t pid = ::fork();
  if (pid < 0) return "error: fork() failed";
  if (pid == 0) {
    // child
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
    ::execl("/bin/bash", "bash", "-c", command.c_str(), (char*)nullptr);
    ::_exit(127);
  }

  // parent
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  int fds[2] = {out_pipe[0], err_pipe[0]};
  bool open[2] = {true, true};
  bool timed_out = false;
  bool interrupted_ = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  char buf[8192];
  std::string out, errout;

  while (open[0] || open[1]) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(pid, SIGKILL);
      timed_out = true;
      break;
    }
    // Cooperative interrupt (Ctrl-C): kill the child and stop promptly so the
    // running turn can return to the prompt instead of waiting out the command.
    if (util::interrupted()) {
      ::kill(pid, SIGKILL);
      interrupted_ = true;
      break;
    }
    int t = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (t < 1) t = 1;
    // Cap the per-iteration poll so a silent long-running command still notices
    // an interrupt promptly (otherwise poll would block until the full timeout).
    if (t > 200) t = 200;
    struct pollfd pf[2];
    int pidx[2];
    int n = 0;
    for (int i = 0; i < 2; ++i) {
      if (open[i]) {
        pf[n] = {fds[i], POLLIN, 0};
        pidx[n] = i;
        ++n;
      }
    }
    int pr = ::poll(pf, n, t);
    if (pr <= 0) continue;
    for (int k = 0; k < n; ++k) {
      if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int i = pidx[k];
      ssize_t r = ::read(fds[i], buf, sizeof buf);
      if (r > 0) {
        if (i == 0)
          out.append(buf, (size_t)r);
        else
          errout.append(buf, (size_t)r);
      } else if (r == 0) {
        open[i] = false;
      } else if (errno != EINTR) {
        open[i] = false;
      }
    }
  }

  ::close(out_pipe[0]);
  ::close(err_pipe[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);

  int code;
  if (WIFEXITED(status)) code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
  else code = -1;

  std::string result = "exit code: " + std::to_string(code);
  if (timed_out) result += " (timed out after " + std::to_string(timeout_s) + "s)";
  if (interrupted_) result += " (interrupted)";
  if (!out.empty()) {
    if (out.size() > kMaxOutput) out = out.substr(0, kMaxOutput) + "... [truncated]";
    result += "\n--- stdout ---\n" + out;
  }
  if (!errout.empty()) {
    if (errout.size() > kMaxOutput) errout = errout.substr(0, kMaxOutput) + "... [truncated]";
    result += "\n--- stderr ---\n" + errout;
  }
  return result;
}

}  // namespace

void register_shell_tools(ToolRegistry& r) {
  r.add("shell",
        "Run a shell command via /bin/bash and return its exit code, stdout, and stderr. "
        "Long-running commands should be used with a timeout.",
        {{"type", "object"},
         {"properties",
          {{"command", {{"type", "string"}, {"description", "Command to run"}}},
           {"cwd", {{"type", "string"}, {"description", "Optional working directory"}}},
           {"timeout_ms",
            {{"type", "integer"}, {"description", "Optional timeout in ms (default 120000)"}}}}},
         {"required", {"command"}},
         {"additionalProperties", false}},
        /*read_only=*/false, do_shell);
}

}  // namespace dog
