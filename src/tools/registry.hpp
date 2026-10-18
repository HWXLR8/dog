#pragma once
#include <functional>
#include <string>
#include <vector>
#include "json/json.hpp"

namespace dog {

// An image to send to the model as an OpenAI `image_url` content part.
// `data` is raw image bytes (PNG/JPEG/...); the harness base64-encodes it.
struct ImagePart {
  std::string mime;
  std::string data;
  std::string path;  // source path, for display
};

// A registry of tools. Each tool is an OpenAI function schema plus an executor.
// `read_only` tools are the only ones allowed to run while in plan mode.
class ToolRegistry {
public:
  // Executors may append produced images to `images` (null allowed).
  using Executor = std::function<std::string(const dog::json&, std::vector<ImagePart>*)>;

  void add(std::string name, std::string description, dog::json params_schema,
           bool read_only, Executor exec);

  // Build the OpenAI "tools" array. When read_only_only is true, only read_only
  // tools are included (used in plan mode).
  dog::json tools_array(bool read_only_only) const;

  // Execute a tool by name. Text result returned; produced images appended to
  // `images` when non-null.
  std::string dispatch(const std::string& name, const dog::json& args,
                       std::vector<ImagePart>* images = nullptr) const;

  bool known(const std::string& name) const;
  bool is_read_only(const std::string& name) const;

private:
  struct Entry {
    std::string name;
    std::string description;
    dog::json params;
    bool read_only;
    Executor exec;
  };
  std::vector<Entry> entries_;
};

}  // namespace dog
