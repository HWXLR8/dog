#include "tools/screenshot.hpp"
#include "util.hpp"
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace dog {

namespace {

// Capture the current screen via `grim` (Wayland) and return it as the same
// "[image:<mime>]<base64>" tag that read_file uses. grim writes the PNG to
// stdout; we pipe it, base64-encode, and the agent loop inlines it as an
// image_url part the model can see.
std::string do_screenshot(const nlohmann::json&) {
  int out_pipe[2];
  if (::pipe(out_pipe) != 0) return "error: pipe() failed";

  pid_t pid = ::fork();
  if (pid < 0) return "error: fork() failed";

  if (pid == 0) {
    // child: redirect stdout to pipe, exec grim which writes a PNG to stdout.
    ::close(out_pipe[0]);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::close(out_pipe[1]);
    const char* argv[] = {"grim", "-", (char*)nullptr};
    ::execvp("grim", const_cast<char* const*>(argv));
    ::_exit(127);
  }

  // parent: read full PNG from pipe.
  ::close(out_pipe[1]);
  std::string png;
  char buf[8192];
  for (;;) {
    ssize_t r = ::read(out_pipe[0], buf, sizeof buf);
    if (r > 0)
      png.append(buf, (size_t)r);
    else if (r == 0 || errno != EINTR)
      break;
  }
  ::close(out_pipe[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (code != 0 || png.empty())
    return "error: screenshot failed (grim exit " + std::to_string(code) + ")";

  return std::string("[image:image/png]") + util::base64_encode(png);
}

}  // namespace

void register_screenshot_tool(ToolRegistry& r) {
  r.add("screenshot",
        "Capture the current screen and return the image so you can see what "
        "something looks like.",
        {{"type", "object"},
         {"properties", nlohmann::json::object()},
         {"additionalProperties", false}},
        /*read_only=*/true, do_screenshot);
}

}  // namespace dog
