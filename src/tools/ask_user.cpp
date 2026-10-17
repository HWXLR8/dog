#include "tools/ask_user.hpp"
#include "ui/ui.hpp"

namespace dog {

void register_ask_user_tool(ToolRegistry& r, Ui& screen, bool interactive) {
  dog::json opt_schema = {
      {"type", "array"},
      {"description", "2-4 choices; the user can also type their own answer"},
      {"items",
       {{"type", "object"},
        {"properties",
         {{"label", {{"type", "string"}}},
          {"description", {{"type", "string"}}}}},
        {"required", {"label"}}}}};
  dog::json header = {{"type", "string"}, {"description", "1-2 word tab label"}};
  dog::json question_str = {{"type", "string"}};
  dog::json multi_sel = {{"type", "boolean"}};
  dog::json q_obj = {
      {"type", "object"},
      {"properties",
       {{"header", header},
        {"question", question_str},
        {"options", opt_schema},
        {"multi_select", multi_sel}}},
      {"required", {"question", "options"}}};
  dog::json questions = {
      {"type", "array"},
      {"description", "1-4 questions, shown together"},
      {"items", q_obj}};
  dog::json props = {
      {"questions", questions},
      {"question", question_str},
      {"options", opt_schema},
      {"multi_select", multi_sel}};
  dog::json params = {{"type", "object"}, {"properties", props}};

  r.add("ask_user",
        "Ask the user multiple-choice questions. Only when blocked on decisions "
        "that are theirs to make (which approach, scope, anything destructive, "
        "missing information) -- not for confirmations or things you can decide. "
        "Put related questions in one call (`questions`, up to 4); a single "
        "question can use `question`/`options` directly.",
        params,
        /*read_only=*/true,
        [&screen, interactive](const dog::json& a) -> std::string {
          if (!interactive)
            return "No user is available (headless run). Choose the most sensible "
                   "option yourself and state the assumption in your answer.";
          auto parse = [](const dog::json& j) -> dog::Ui::Question {
            dog::Ui::Question q;
            q.header = j.value("header", "");
            q.question = j.value("question", "");
            q.multi = j.value("multi_select", false);
            if (j.contains("options") && j["options"].is_array())
              for (auto& o : j["options"]) {
                if (o.is_string())
                  q.opts.push_back({o.get<std::string>(), ""});
                else if (o.is_object())
                  q.opts.push_back({o.value("label", ""), o.value("description", "")});
              }
            if (q.opts.size() > 9) q.opts.resize(9);
            return q;
          };
          std::vector<dog::Ui::Question> qs;
          if (a.contains("questions") && a["questions"].is_array())
            for (auto& j : a["questions"])
              if (j.is_object()) qs.push_back(parse(j));
          if (qs.empty() && a.contains("question")) qs.push_back(parse(a));
          if (qs.size() > 4) qs.resize(4);
          for (auto& q : qs)
            if (q.question.empty() || q.opts.empty())
              return "error: ask_user needs each question to have text and options";
          if (qs.empty()) return "error: ask_user needs a question and options";
          auto ans = screen.ask(qs);
          if (ans.empty()) return "(the user interrupted instead of answering)";
          if (qs.size() == 1) return "User answered: " + ans[0];
          std::string res = "User answered:";
          for (size_t i = 0; i < qs.size(); i++)
            res += "\n" + std::to_string(i + 1) + ". " + qs[i].question +
                   " -> " + ans[i];
          return res;
        });
}

}  // namespace dog
