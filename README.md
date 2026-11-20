<h1>KritaAI <i>— an AI chat docker for Krita</i></h1>

**English** | [中文](README-zh.md)

✨[Features](#features) | 🛠️[Getting Started](#getting-started) | ⚙️[Configure](#configure) | 🔒[Privacy](#privacy) | 🧩[Development](#development) | 🐞[Issues](https://github.com/dreamldx/KritaAI/issues)

This is a C++ plugin that puts an **AI Chat** panel into [Krita](https://krita.org). You talk to any
OpenAI-compatible model, and the model can use tools to look at your document, change its layers and
selection, and send the picture to Topaz (upscale / denoise) through [fal.ai](https://fal.ai).

The main goals of this project are:
* **Chat that can act.** The AI is not only a text box. It reads the open document, asks a vision model
  where things are, selects them and edits layers, so you can say what you want instead of hunting for menus.
* **Nothing you cannot undo.** Every change the AI makes (layer, selection, Topaz result) is one step of
  Krita's own undo history. The result of a Topaz job always goes into a new layer; your pixels are never overwritten.
* **Your server, your keys.** Bring any OpenAI-compatible server: a hosted API, OpenRouter, or a local one.
  Messages go only to the server you set up. Nothing else is contacted.

## <a name="features"></a> Features

* **Chat panel**: message cards with icon, name and markdown body; the footer shows the tokens used and the seconds the AI needed.
* **Any OpenAI-compatible server**: `POST {server}/chat/completions` with streaming (SSE), tool calls and an optional `reasoning_effort`.
* **Markdown answers**: headings, lists, tables, code, quotes, links. Raw HTML in an answer is shown as text, and only `http`, `https` and `mailto` links open.
* **Responsive**: the AI loop, the network and the database run in a worker thread, so Krita never waits for the AI.
* **History**: the chat is kept in SQLite across sessions.
* **Document tools**: `get_document_info` (size, resolution, color model, layers with ids and blend modes, selection) and `ask_about_image`, which shrinks the canvas to at most 1024 × 1024 and asks a *vision model*. Asked where something is, the vision model answers with boxes `[left, top, right, bottom]` as fractions 0 to 1.
* **Selection**: `box_select` selects a rectangle given as fractions 0 to 1 (numbers above 1 are refused).
* **Layer tools**: `add_layer`, `add_group`, `rename_layer`, `set_layer_visibility`, `set_layer_opacity`, `set_layer_blend_mode`, `move_layer`, `duplicate_layer`, `delete_layer`, `reorder_layer`, `move_layer_to_group`, `scale_image`.
* **Topaz through fal.ai**: `upscale_image` and `denoise_image`. The picture (or the region) is uploaded, processed, and the result goes into a **new layer**. A modal progress window with a Cancel button is shown while the job runs, and the uploaded file is deleted from fal.ai afterwards.
* **Undo**: each of these changes is one step of *Edit → Undo*.
* **Log file**: timings and token counts of every request, to find out why something is slow.

Tools are offered only when they can work: `ask_about_image` needs a vision model, the Topaz tools need a fal.ai key.

## <a name="getting-started"></a> Getting Started

### Requirements

<table>
<tr><td>Krita</td><td>a source tree of Krita (master branch) that builds; the plugin is a C++ plugin and has to be built together with Krita</td></tr>
<tr><td>Qt</td><td>5.15 (Core, Gui, Widgets, Network, Sql with the SQLite driver)</td></tr>
<tr><td>Platform</td><td>developed on Windows with the official Krita CI dependencies and the llvm-mingw UCRT toolchain; nothing in the code is Windows-specific</td></tr>
<tr><td>AI server</td><td>any OpenAI-compatible chat server; optionally a vision model and a fal.ai key</td></tr>
</table>

### Installation

There is no ready-made download: a C++ plugin has to match the Krita it is loaded into. The repository
mirrors Krita's folder layout, so you copy it over a Krita source tree:

1. Copy the plugin into your Krita sources:
   ```bash
   # from the root of this repository
   cp -r plugins/dockers/aichat  /path/to/krita/plugins/dockers/
   ```
2. Add one line to `plugins/dockers/CMakeLists.txt` of Krita:
   ```cmake
   add_subdirectory(aichat)
   ```
3. Configure and build Krita as usual. The target is `kritaaichatdocker`, installed with the other Krita plugins.
4. Start Krita and show the panel: *Settings → Dockers → AI Chat*.
5. Fill in the server in *Settings → Configure Krita → AI Chat* (see [Configure](#configure)).

> [!NOTE]
> A C++ plugin is loaded once when Krita starts, so a new build needs a restart of Krita.

### _Optional:_ Vision model

Set a vision model in the settings to let the AI look at the picture (`ask_about_image`). It then answers
where things are as fractions, which the AI can pass to `box_select`. The vision model may live on a
different server than the chat model.

### _Optional:_ Topaz upscale and denoise

Set a [fal.ai](https://fal.ai) key. The key needs the `assets:write` permission, so that uploaded pictures
can be deleted after the job.

## <a name="configure"></a> Configure

*Settings → Configure Krita → AI Chat*

| Field | Meaning |
|---|---|
| Server address | The address of the server up to and including `/v1`, e.g. `https://api.openai.com/v1` or `https://openrouter.ai/api/v1`. `/chat/completions` is added. |
| API key | Not needed by some local servers. Stored **unencrypted** in Krita's `kritarc`. |
| Model | The model name, e.g. `gpt-4o-mini`. |
| Send the thinking effort | Sends the *Thinking* choice of the panel as `reasoning_effort`. Leave it off if your server rejects the field. |
| Vision model (address, key, model) | Optional. A model that can read pictures, used by `ask_about_image`. May be on a different server than the chat model. |
| Ask the vision model not to think | Sends `"reasoning": {"enabled": false}` (OpenRouter style). On by default; turn it off if the server rejects it. |
| Blocked providers | OpenRouter only: company names (comma separated) that must not serve the vision requests; sent as `provider.ignore`. Useful when one provider is slow. |
| fal.ai key | Needed for the Topaz tools. |

Where things are stored, on Windows `%APPDATA%\krita\aichat\` (Krita's data folder on other systems):

* `chat.sqlite`: the chat history.
* `aichat.log`: one line per request with timings, token counts and the provider that served it. API keys and pictures are never logged.

The settings are in Krita's `kritarc` (`aichat_*` keys).

## <a name="privacy"></a> Privacy

* The messages go to the server you configured; nothing else is contacted.
* With a vision model set up, the AI can send **a shrunk picture of your open document** to that server whenever it decides to look. Do not set one up if that is not acceptable.
* With a fal.ai key set, the Topaz tools upload **the picture (or the region)** to fal.ai's file storage and delete it after the job. They are only used when you ask, and they cost money.
* The API keys are saved in plain text in `kritarc`.

## <a name="status"></a> Status

Done: chat, streaming, markdown, history, settings, document and vision tools, layer and selection tools, Topaz upscale / denoise through fal.ai, undo for every change.

Not done yet:

* Confirmation cards before the paid Topaz tools and the destructive layer tools (`delete_layer`, `scale_image`); for now only the tool descriptions tell the AI to use them when the user asks.
* Tool calls are not stored in the database and have no card in the chat.
* Models that edit part of a picture from a mask (inpainting, outpainting): Topaz takes no mask.
* The *Add image* button only emits a signal.
* A list of old conversations: they are kept in the database, but only the latest is shown.
* Cutting a very long conversation: the whole conversation is sent with every request.

## <a name="development"></a> Development

Contributions are welcome. Read [`plugins/dockers/aichat/AGENTS.md`](plugins/dockers/aichat/AGENTS.md) first: it describes the design, the threading rules, the pitfalls and the coding conventions (those of Krita's HACKING).

All sources are in `plugins/dockers/aichat/`, in `namespace AiChat`.

| Folder / file | What it does |
|---|---|
| `AiChat.*` | The plugin: registers the docker and the settings page. |
| `ai/Agent.*` | The AI loop: ask, run the tools the model asks for, ask again (at most 10 rounds). |
| `ai/tools/` | The tools. `Tools.*` is the registry and the executor that runs a function in the GUI thread safely; `DocumentTools.*` (`get_document_info`, `ask_about_image`), `LayerOps.*` / `LayerTools.*` (layers, selection, undo), `Fal.*` / `FalTools.*` (Topaz through fal.ai), `Vision.*` (the vision model request). |
| `network/` | `Client.*`: one streamed HTTP request; `Stream.*`: SSE parsing, collecting text, tool calls and token usage. |
| `config/Config.*` | The settings stored in `kritarc`. |
| `model/` | The state of the chat, with no widget: `Chat.*` (history loaded, busy, thinking effort, the text so far; `send`, `stop`, `newChat`; owns the worker thread and the tools) and `Request.*` (turns the settings into a request). |
| `ui/` | The views: `Dock.*` (the panel: shows a `Chat`; message cards, markdown, input box), `MessageWidget.h` (the cards and buttons), `Preferences.*` (the page in *Configure Krita*), `Progress.*` (the modal progress window of the Topaz tools). |
| `utils/` | `Worker.*` (lives in the worker thread; owns the AI loop and the database), `Store.*` (SQLite with schema migrations), `Log.*` (the log file). |

Adding a tool means adding a `Tool` (name, description, JSON schema, handler) to the registry. A handler runs in the worker thread and reaches Krita's image only through `GuiExecutor::run`.

### Tests

The tests need no network and no real AI service: they use a small scripted HTTP server that is part of the tests.

```bash
ctest -R aichat --output-on-failure
```

There are nine test programs: chat model, panel, settings, database, stream parsing, worker, tools, fal.ai client and layer operations (the layer tests use a real Krita image and its undo history).

## Technology

* Host application: [Krita](https://krita.org) (C++, Qt 5.15)
* Chat backend: any OpenAI-compatible [chat completions](https://platform.openai.com/docs/api-reference/chat) API, streamed with SSE
* Vision: any OpenAI-compatible model that accepts images
* Upscale and denoise: [Topaz](https://www.topazlabs.com) models served by [fal.ai](https://fal.ai)
* Storage: SQLite through Qt

## License

GPL-3.0-or-later, the same as Krita. See [LICENSE](LICENSE).
