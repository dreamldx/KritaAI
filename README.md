# KirtaAI — an AI chat docker for Krita

A C++ plugin for [Krita](https://krita.org) that adds an **AI Chat** panel. You talk to any
OpenAI-compatible model from inside Krita, and the model can look at your document.
Later steps (not done yet) let it edit the image.

> 中文简介：这是 Krita 的 C++ 插件，在 Krita 里增加一个 AI 对话面板。支持任何 OpenAI 兼容接口
> （OpenAI、OpenRouter、DeepSeek、本地 Ollama 等），流式输出、Markdown 渲染、SQLite 保存聊天历史，
> AI 可以通过工具读取当前文档信息，并通过视觉模型“看”画布。设置在 *设置 → 配置 Krita → AI Chat*。

## Features

- **Chat panel** with message cards: icon, name, markdown body, and a footer with the tokens used and the seconds the AI needed.
- **Any OpenAI-compatible server**: `POST {server}/chat/completions` with streaming (SSE), tool calls and an optional `reasoning_effort`.
- **Markdown answers**: headings, lists, tables, code, quotes, links. Raw HTML in an answer is shown as text, and only `http`, `https` and `mailto` links open.
- **Runs off the GUI thread**: the AI loop, the network and the database live in a worker thread, so Krita never waits for the AI.
- **Chat history in SQLite**, kept across sessions.
- **Settings inside Krita**: a page *AI Chat* in *Configure Krita* (server address, API key, model, vision model).
- **Tools the AI can use** (read-only for now):
  - `get_document_info`: name, size, resolution, color model, layers and the selection of the open document.
  - `ask_about_image(question, region?, selection_only?)`: the canvas is sent, shrunk to at most 1024 × 1024, to a *vision model* and its text answer goes back to the chat model. Only offered when a vision model is set up.
- **A log file** with timings and token counts of every request, for finding out why something is slow.

## Requirements

- A Krita source tree (master branch) that builds. The plugin uses Krita's own libraries (`kritaui`) and Qt 5.15 (Core, Gui, Widgets, Network, Sql with the SQLite driver).
- It was developed on Windows with the official Krita CI dependencies and the llvm-mingw UCRT toolchain. Nothing in the code is Windows-specific.

## Install

The repository mirrors Krita's folder layout, so you can copy it over a Krita source tree:

```bash
# from the root of this repository
cp -r plugins/dockers/aichat  /path/to/krita/plugins/dockers/
```

Then add one line to `plugins/dockers/CMakeLists.txt` of Krita:

```cmake
add_subdirectory(aichat)
```

Configure and build Krita as usual. The target is `kritaaichatdocker`, a plugin that is installed with the other Krita plugins. Start Krita, open *Settings → Dockers → AI Chat*.

A C++ plugin is loaded once when Krita starts, so a new build needs a restart of Krita.

## Configure

*Settings → Configure Krita → AI Chat*

| Field | Meaning |
|---|---|
| Server address | The address of the server up to and including `/v1`, e.g. `https://api.openai.com/v1` or `https://openrouter.ai/api/v1`. `/chat/completions` is added. |
| API key | Not needed by some local servers. Stored **unencrypted** in Krita's `kritarc`. |
| Model | The model name, e.g. `gpt-4o-mini`. |
| Send the thinking effort | Sends the *Thinking* choice of the panel as `reasoning_effort`. Leave it off if your server rejects the field. |
| Vision model (address, key, model) | Optional. A model that can read pictures, used by `ask_about_image`. May be a different server from the chat model. |
| Ask the vision model not to think | Sends `"reasoning": {"enabled": false}` (OpenRouter style). On by default; turn it off if the server rejects it. |

## Where things are stored

On Windows `%APPDATA%\krita\aichat\` (Krita's data folder on other systems):

- `chat.sqlite` — the chat history.
- `aichat.log` — one line per request: timings, token counts, the provider that served the request. API keys and pictures are never logged.

The settings are in Krita's `kritarc` (`aichat_*` keys).

## Privacy

- The messages go to the server you configured, nothing else is contacted.
- With a vision model set up, the AI can send **a shrunk picture of your open document** to that server whenever it decides to look. Do not set one if that is not acceptable.
- The API key is saved in plain text in `kritarc`.

## Code layout

All sources are in `plugins/dockers/aichat/`.

| File | What it does |
|---|---|
| `AiChat.*` | The plugin: registers the docker and the settings page. |
| `AiChatDock.*` | The panel: message cards, markdown, input box, toolbar. |
| `AiChatWorker.*` | Lives in the worker thread: owns the AI loop and the database. |
| `AiChatAgent.*` | The AI loop: ask, run the tools the model asks for, ask again (at most 10 rounds). |
| `AiChatClient.*`, `AiChatStream.*` | One streamed HTTP request; SSE parsing; collecting text, tool calls and token usage. |
| `AiChatTools.*` | Tool registry and the executor that runs a function in the GUI thread safely. |
| `AiChatDocumentTools.*` | `get_document_info` and `ask_about_image`; the access to Krita's document. |
| `AiChatVision.*` | The request to the vision model. |
| `AiChatStore.*` | SQLite storage with schema migrations. |
| `AiChatConfig.*`, `AiChatPreferences.*` | Settings and their page in *Configure Krita*. |
| `AiChatLog.*` | The log file. |

Adding a tool means adding an `AiChatTool` (name, description, JSON schema, handler) to the registry. A handler runs in the worker thread and reaches Krita's image only through `AiChatGuiExecutor::run`.

## Tests

The tests need no network and no real AI service: they use a small scripted HTTP server that is part of the test.

```bash
ctest -R aichat --output-on-failure
```

Six test programs: panel, settings, database, stream parsing, tools and worker.

## Status

Done: chat, streaming, markdown, history, settings, read-only tools, vision question about the canvas.

Not done yet:

- Tools that change the image (denoise, upscale, selection edit, outpainting). A plan exists to call [fal.ai](https://fal.ai) Topaz models through its queue API.
- The *Add image* button only emits a signal.
- A list of old conversations; conversations are kept in the database but only the latest is shown.
- Cutting a very long conversation: the whole conversation is sent with every request.

## License

GPL-3.0-or-later, the same as Krita. See [LICENSE](LICENSE).
