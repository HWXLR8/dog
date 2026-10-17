#pragma once
#include "tools/registry.hpp"

namespace dog {
class Ui;
// Register the ask_user tool: the agent asks the user multiple-choice questions,
// answered by a UI picker. One question, or several shown as tabs with a Submit
// tab. Always registered (the tools array must not change mid-session).
void register_ask_user_tool(ToolRegistry& r, Ui& screen, bool interactive);
}  // namespace dog
