# dog

A minimal terminal coding agent.

## Build

```sh
make        # produces ./dog
make clean  # remove objects and binary
```

Requires g++ (C++20) and libcurl.

## Usage

```sh
dog                      # interactive session
dog "fix the bug"        # one-shot (headless), exits after one turn
dog --plan "design X"    # start in plan mode
dog -c myconfig.json     # use an alternate config file
```

### Interactive commands

| Command | Effect |
|---------|--------|
| `/help`   | list commands |
| `/clear`  | clear the conversation context |
| `/plan`   | toggle plan mode |
| `/exit`   | quit (or `Ctrl-D`) |

Attach images by name in a prompt: `look at @screenshot.png and describe it`.
Interrupt a running turn with `Ctrl-C`.

## Configuration

Config is read from `dog.json` (override the path with `-c`). Environment
variables are read first; the config file then overrides them.

### Example `dog.json`

```json
{
  "model": {
    "base_url": "http://localhost:8000/v1",
    "api_key": "...",
    "model": "my-model",
    "max_turns": 500,
    "context_window": 192000,
    "compact_threshold": 0.75,
    "compact_keep": 6
  },
  "skills_dir": "skills",
  "system_file": "SYSTEM.md",
  "mcp": []
}
```

### Keys

| Key | Default | Description |
|-----|---------|-------------|
| `model.base_url` | `http://localhost:8000/v1` | OpenAI-compatible endpoint |
| `model.api_key` | *(empty)* | sent as `Authorization: Bearer ...` |
| `model.model` | `local-model` | model name |
| `model.max_turns` | `500` | max tool/agent turns per request |
| `model.context_window` | `128000` | token window (0 = unknown) |
| `model.compact_threshold` | `0.75` | compact when prompt tokens exceed this fraction of the window |
| `model.compact_keep` | `6` | messages kept verbatim when compacting |
| `skills_dir` | `skills` | directory of `SKILL.md` skills |
| `system_file` | `SYSTEM.md` | system prompt file (built-in fallback if missing) |
| `mcp` | `[]` | MCP servers: `{ name, transport, command, args, url }` |

### Environment variables

- `OPENAI_BASE_URL` — endpoint (maps to `model.base_url`)
- `OPENAI_API_KEY` — API key (maps to `model.api_key`)
- `OPENAI_MODEL` — model name (maps to `model.model`)
- `HARNESS_SKILLS_DIR` — skills directory (`skills_dir`)
- `HARNESS_SYSTEM_FILE` — system prompt file (`system_file`)
- `HARNESS_CONTEXT_WINDOW` — token window (`model.context_window`)
- `HARNESS_MAX_TURNS` — max turns per request (`model.max_turns`)
- `HARNESS_COMPACT_THRESHOLD` — compact threshold (`model.compact_threshold`)
- `HARNESS_COMPACT_KEEP` — messages kept when compacting (`model.compact_keep`)
