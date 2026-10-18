#include "tools/screenshot.hpp"
#include "util.hpp"
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace dog {

namespace {

// Capture the current screen via `grim` (Wayland) and hand the raw PNG bytes to
// the agent loop as an image part (inlined as an image_url the model can see).
std::string do_screenshot(const dog::json&, std::vector<ImagePart>* images) {
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

  if (images) {
    ImagePart im;
    im.mime = "image/png";
    im.data = std::move(png);
    im.path = "screenshot";
    images->push_back(std::move(im));
  }
  return "[image attached]";
}

}  // namespace

void register_screenshot_tool(ToolRegistry& r) {
  r.add("screenshot",
        "Capture the current screen and return the image so you can see what "
        "something looks like.",
        {{"type", "object"},
         {"properties", dog::json::object()},
         {"additionalProperties", false}},
        /*read_only=*/true, do_screenshot);
}

}  // namespace dog
