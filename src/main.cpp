#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include "agent/loop.hpp"
#include "config/config.hpp"
#include "mcp/mcp.hpp"
#include "tools/task.hpp"
#include "tools/tools.hpp"
#include "ui/tui.hpp"
#include "util.hpp"

namespace {

// Loads the agent's system prompt from an external file (default "SYSTEM.md").
// Falls back to a minimal built-in prompt if the file can't be read, so the
// dog still starts with a usable (if sparse) instruction set.
std::string read_system_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    std::cerr << "warning: could not read system prompt file '" << path
              << "'; using built-in fallback\n";
    return "You are a minimal, practical coding agent running in a terminal on Linux.";
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void print_usage() {
  std::cout << "usage: dog [options] [prompt]\n"
            << "  (no prompt)      interactive session\n"
            << "  \"prompt\"         one-shot headless run\n"
            << "  --plan           start in plan mode\n"
            << "  -c, --config F   config file (default ~/.config/dog/dog.json)\n"
            << "  -h, --help       this help\n";
}

// Pull image attachments out of a line. A token starting with '@' that names an
// existing image file (e.g. @img.png, @/tmp/screenshot.jpg) is turned into an
// ImagePart and removed from the text. The returned text is what the model reads
// as the user's words.
std::vector<dog::ImagePart> extract_image_attachments(std::string& text) {
  std::vector<dog::ImagePart> parts;
  std::string out;
  size_t i = 0;
  auto push = [&](const std::string& tok) {
    std::string p = tok;
    if (!p.empty() && p[0] == '@') p = p.substr(1);
    p = dog::util::expand_home(p);
    if (dog::util::is_image_file(p)) {
      std::ifstream f(p, std::ios::binary);
      if (!f.good()) {
        std::cout << "\x1b[2m(image not readable: " << p << ")\x1b[0m\n";
        return;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      std::string data = ss.str();
      if (data.empty()) {
        std::cout << "\x1b[2m(image is empty: " << p << ")\x1b[0m\n";
        return;
      }
      std::string mime = dog::util::sniff_image_mime(data);
      if (mime.empty()) mime = dog::util::mime_from_path(p);
      if (!mime.empty()) {
        dog::ImagePart ip;
        ip.mime = mime;
        ip.data = std::move(data);
        ip.path = p;
        parts.push_back(std::move(ip));
      }
    }
  };
  while (i < text.size()) {
    char c = text[i];
    if (c == ' ') {
      size_t j = i;
      while (j < text.size() && text[j] == ' ') j++;
      out.append(text, i, j - i);
      i = j;
    } else if (c == '@') {
      size_t j = i + 1;
      while (j < text.size() && text[j] != ' ') j++;
      push(text.substr(i, j - i));
      i = j;
    } else {
      out.push_back(c);
      i++;
    }
  }
  text = std::move(out);
  return parts;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = dog::Config::default_config_path();
  std::string one_shot;
  bool plan = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else if (a == "--plan") {
      plan = true;
    } else if (a == "--config" || a == "-c") {
      if (i + 1 < argc)
        config_path = argv[++i];
      else {
        std::cerr << "error: --config needs a path\n";
        return 2;
      }
    } else if (!a.empty() && a[0] != '-') {
      one_shot = a;
      break;
    } else {
      std::cerr << "error: unknown option: " << a << "\n";
      return 2;
    }
  }

  bool interactive = one_shot.empty();
  dog::Config cfg = dog::Config::load(config_path);

  // The UI owns all rendering; agent/tool/MCP output flows through it.
  dog::Tui tui(cfg, interactive);
  auto log = [&tui](const std::string& s) { tui.on_system(s); };

  dog::ToolRegistry reg;
  dog::register_core_tools(reg, cfg.skills_dir);
  dog::register_task_tool(reg, cfg, cfg.skills_dir, log);

  dog::McpRegistry mcp;
  mcp.start(cfg.mcp, reg, log);

  dog::UiCallbacks ui;
  ui.text = [&tui](const std::string& s) { tui.on_text(s); };
  ui.reasoning = [&tui](const std::string& r) { tui.on_reasoning(r); };
  ui.model_start = [&tui]() { tui.on_model_start(); };
  ui.round_end = [&tui]() { tui.on_round_end(); };
  ui.usage = [&tui](long a, long b, long c) { tui.on_usage(a, b, c); };
  ui.status = [&tui](const std::string& s) { tui.on_status(s); };
  ui.images = [&tui](const std::vector<dog::ImagePart>& imgs) { tui.on_images(imgs); };
  ui.tool_call = [&tui](const std::string& n, const std::string& a, const std::string& r) {
    tui.on_tool_call(n, a, r);
  };
  ui.system = [&tui](const std::string& s) { tui.on_system(s); };
  ui.confirm = [&tui](const std::string& p) { return tui.confirm(p); };

  dog::AgentLoop agent(cfg, &reg, read_system_file(cfg.system_file), plan, std::move(ui));

  if (!interactive) {
    std::string text = one_shot;
    std::vector<dog::ImagePart> attached = extract_image_attachments(text);
    bool ok = agent.run_turn(text, attached.empty() ? nullptr : &attached);
    return ok ? 0 : 1;
  }

  for (;;) {
    std::string line = tui.read_input();
    if (tui.exit_requested()) break;
    std::string t = dog::util::trim(line);
    if (t.empty()) continue;
    if (t == "/exit" || t == "/quit") break;
    if (t == "/help") {
      tui.on_system("commands: /help  /clear  /plan  /exit");
      continue;
    }
    if (t == "/clear") {
      agent.clear_context();
      tui.clear_transcript();
      tui.on_system("context cleared");
      continue;
    }
    if (t == "/plan") {
      if (agent.in_plan_mode())
        agent.exit_plan_mode();
      else
        agent.enter_plan_mode();
      continue;
    }
    tui.user_message(t);
    {
      // Extract @-referenced image attachments before the turn; the text keeps
      // only the user's words.
      std::vector<dog::ImagePart> attached = extract_image_attachments(t);
      // Start each turn with a clear interrupt flag; the TUI sets it on Ctrl-C.
      dog::interrupt_flag.store(false);
      std::atomic<bool> done{false};
      std::thread th([&] {
        agent.run_turn(t, attached.empty() ? nullptr : &attached);
        done = true;
      });
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)tui.poll_input();  // absorbs keystrokes; Ctrl-C sets the flag
      }
      th.join();
      dog::interrupt_flag.store(false);
    }
  }
  return 0;
}
