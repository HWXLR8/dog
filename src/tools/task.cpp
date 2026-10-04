#include "tools/task.hpp"
#include "agent/loop.hpp"
#include "tools/tools.hpp"
#include "util.hpp"

namespace dog {

namespace {

const int kSubagentMaxTurns = 15;
const size_t kMaxSubagentOutput = 20000;

const char* kSubagentSystem =
    "You are a focused subagent inside a coding dog. Complete the task using your "
    "tools (read_file, write_file, edit_file, shell, glob, grep, web_fetch). Do not ask "
    "questions; make reasonable decisions. When finished, give a clear, complete final "
    "answer as plain text.";

std::string do_task(const Config& cfg, const std::string& skills_dir,
                    const nlohmann::json& args,
                    std::function<void(const std::string&)> log) {
  std::string description = args.value("description", "");
  std::string prompt = args.value("prompt", "");
  if (prompt.empty()) return "error: prompt is required";

  // A fresh registry with only the core tools: no MCP (avoids nested server spawns),
  // no task (avoids infinite recursion).
  ToolRegistry sub_reg;
  register_core_tools(sub_reg, skills_dir);

  Config sub_cfg = cfg;
  sub_cfg.max_turns = kSubagentMaxTurns;

  UiCallbacks sub_ui;  // silent: the subagent's output is captured, not streamed
  AgentLoop sub(sub_cfg, &sub_reg, kSubagentSystem, /*start_in_plan=*/false, sub_ui);

  std::string label = description.empty() ? prompt : description;
  if (log) log("task: " + util::truncate(label, 80));

  sub.run_turn(prompt);
  std::string out = sub.last_assistant_text();
  if (out.empty()) return "(subagent produced no output)";
  return util::truncate(out, kMaxSubagentOutput);
}

}  // namespace

void register_task_tool(ToolRegistry& r, const Config& cfg, const std::string& skills_dir,
                        std::function<void(const std::string&)> log) {
  r.add("task",
        "Delegate a self-contained subtask to a subagent. The subagent runs its own "
        "model loop with the core tools (read_file, write_file, edit_file, shell, glob, "
        "grep, web_fetch) to completion and returns its final answer. Use it for bounded, "
        "well-specified work; give a complete, self-contained prompt.",
        {{"type", "object"},
         {"properties",
          {{"description", {{"type", "string"},
                             {"description", "Short label for the subtask"}}},
           {"prompt", {{"type", "string"},
                        {"description", "Full, self-contained instructions for the "
                                         "subagent"}}}}},
         {"required", {"prompt"}},
         {"additionalProperties", false}},
        /*read_only=*/false,
        [cfg, skills_dir, log](const nlohmann::json& args) {
          return do_task(cfg, skills_dir, args, log);
        });
}

}  // namespace dog
