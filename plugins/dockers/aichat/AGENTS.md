# AI Chat plugin: design and things to watch out for

`plugins/dockers/aichat` is a C++ docker plugin for Krita: a chat panel on the right that talks to an
OpenAI-compatible service. The AI uses "tools" to read and change the document that is open in Krita.
The goal is an AI that can edit pictures (whole-image denoise/upscale, selections, layer operations, ...).

## 1. Layout

```
aichat/
├── AiChat.h/.cpp        plugin entry: AiChatPlugin, AiChatDockFactory (global namespace)
├── CMakeLists.txt       static library kritaaichatdocker_static + a very thin MODULE kritaaichatdocker
├── kritaaichatdocker.json
├── ai/                  the agent loop
│   ├── Agent.*          class Agent, AgentToolProvider (interface)
│   └── tools/           the tool layer
│       ├── Tools.*          Tool, ToolRegistry, GuiExecutor
│       ├── DocumentTools.*  get_document_info, ask_about_image, DocumentAccess
│       ├── LayerOps.*       layer/selection operations on a KisImage, one undo step per command
│       ├── LayerTools.*     registers the layer tools and box_select
│       ├── Fal.*            fal.ai client (queue API, upload, delete) + FalSettings
│       ├── FalTools.*       upscale_image, denoise_image
│       └── Vision.*         vision model client + VisionSettings
├── config/Config.*      struct Config: reads/writes the settings in kritarc
├── network/             the chat request
│   ├── Client.*         Client, ClientResult, RequestSettings: POST /chat/completions, SSE streaming
│   └── Stream.*         ToolCall, SseParser, CompletionAccumulator, httpErrorMessage
├── ui/                  Dock (chat panel), Preferences (settings page), Progress (modal window of the Topaz tools)
├── utils/               Log (writeLog), Store (SQLite history + StoredMessage), Worker (worker-thread object)
└── tests/               one test program per module; FakeOpenAiServer.h is a scripted fake server
```

Conventions:

- Everything except `AiChat.*` is in `namespace AiChat`; the tests use `using namespace AiChat;`.
- There is no shared `Types.h`: each struct is defined in the header that really uses it (`StoredMessage` in
  `Store.h`, `ToolCall` in `Stream.h`, `VisionSettings`/`FalSettings` in their client headers,
  `RequestSettings` in `Client.h`). `RequestSettings` contains `VisionSettings` and `FalSettings`, so
  `network/Client.h` includes `ai/tools/Fal.h` and `Vision.h` (network depends on ai/tools; a
  known wart).
- No file or class has a prefix (only `AiChat.*`, the plugin entry, keeps its name).

## 2. Threading model (the most important part)

- **GUI thread**: `Dock`, `Progress`, and every access to a Krita document.
- **Worker thread** (`Worker` lives in a QThread): all network and database work, including the agent loop and
  the execution of tools.
- Tools run in the worker thread and **must not touch the Krita document directly**. They go through
  `GuiExecutor::run(fn, &error)`, which runs the function in the GUI thread and waits for it.
  - Do not use `BlockingQueuedConnection`: it deadlocks when the dock is destroyed. The executor has `close()`;
    the destructor closes it first, which releases anyone who is waiting.
- Vision and fal requests wait for the network with a nested `QEventLoop` in the worker thread. Canceling goes
  through `cancel()`: `ToolRegistry` keeps a list of cancel hooks (`setCancelHook` replaces, `addCancelHook`
  appends).
- The progress window `Progress` lives in the GUI thread. The worker drives it with queued calls
  (`beginLater/updateLater/endLater`); its Cancel button emits `cancelRequested`, which is connected with a
  DirectConnection to `cancel()` of the fal client.

## 3. Agent loop and tools

1. The user sends a message -> `Worker::submit` -> `Agent` asks the model with the message history and the tool
   definitions.
2. The model answers with `tool_calls` -> `ToolRegistry::runTool` runs them one after the other -> the results
   go back as `tool` messages -> ask again, until the model gives a final answer.
3. On cancel or failure the partial answer is saved; error cards are not stored; tool calls are not stored in the
   database yet and have no card in the UI.

Availability of a tool: `Tool::isAvailable(settings)`. Without a vision model `ask_about_image` is not offered,
without a fal key the Topaz tools are not offered.

### Tools

| Tool | Notes |
|---|---|
| `get_document_info` | Document size, color, layers (with `id` and `blend_mode`), bounds of the selection. Read-only. |
| `ask_about_image(question, region?, selection_only?)` | Renders the picture (longest side 1024, PNG on a white background) and sends it to a **separate** vision model; the text answer is the tool result (tool messages cannot carry images). When positions are asked for, the vision model is told to answer with boxes `[left, top, right, bottom]` as fractions 0 to 1. The result text says which area was looked at and the `region` to pass on to `box_select`. |
| `box_select(left, top, right, bottom, region?)` | A rectangular selection that replaces the current selection. The four numbers must be fractions 0 to 1: **above 1 or below 0 is refused**, as are `left >= right` and `top >= bottom`. `region` (pixels) says which area the fractions are relative to; default: the whole image. One undo step. |
| layer tools | `add_layer`, `add_group`, `rename_layer`, `set_layer_visibility`, `set_layer_opacity`, `set_layer_blend_mode`, `move_layer`, `duplicate_layer`, `delete_layer`, `reorder_layer`, `move_layer_to_group`, `scale_image`. A layer is addressed by id (best) or name; a name that is not unique is an error that lists the ids. |
| `upscale_image`, `denoise_image` | Topaz through fal.ai. They cost money; the descriptions say "only when the user asks". |

### Undo (requirement: every operation is in the history)

- Each layer/selection command is **one** undo step (`Edit > Undo`). The tools build a `LayerCommand`
  and hand it to `DocumentAccess::applyLayerCommand` (the only entry into the GUI thread);
  `LayerOps` carries it out.
- Krita mechanisms used: `KisImageLayerAdd/Move/RemoveCommand` + `image->undoAdapter()`; property commands go
  through `KisProcessingApplicator` (`KisNodeRenameCommand`, `KisNodeOpacityCommand`,
  `KisNodeCompositeOpCommand`, `KisNodeMoveCommand2`, `KisSetGlobalSelectionCommand`); visibility uses
  `KisLayerPropertiesIcons::setNodePropertyAutoUndo`; scaling uses `image->scaleImage(..., "Bicubic")`.
- The tests use `KisSurrogateUndoStore` to check "one command = one undo step".
- Locked layers cannot be deleted or moved; a group cannot be moved into itself; `scale_image` is limited to
  200 MP; `compositeOp(id)` silently falls back to normal for an unknown id, so check with `hasCompositeOp`
  first.

### Where a Topaz result goes

- Whole picture upscaled: the **document** is scaled to the size of the result first (all layers scale along),
  then the result is added as a new layer at (0,0) named "Upscaled xN".
- A region or selection: the result is scaled back to the size of the region and put at the origin of the
  region; the document size does not change.
- A result bigger than 100 MP is refused before anything is sent. When the user cancels, the tool result is
  "The user canceled the job... Do not start it again unless they ask."

## 4. fal.ai (Topaz)

- Queue API: `POST https://queue.fal.run/<endpoint>` (`topaz/upscale/image/precision`,
  `topaz/denoise/image`), header `Authorization: Key <key>`. The answer of the submit has `request_id`,
  `status_url`, `response_url`, `cancel_url`; **poll the addresses the answer gives**. Statuses are `IN_QUEUE`,
  `IN_PROGRESS`, `COMPLETED`; the result is `{"image":{"url":...}}`. The status/result paths use only the
  first two parts of the endpoint (`topaz/upscale`, `topaz/denoise`).
- **Upload first, then submit.** A 10 MB PNG as a data URI inside the job makes fal hang up and Qt only says
  `Unable to write`. The upload has two steps:
  `POST https://rest.alpha.fal.ai/storage/upload/initiate?storage_type=fal-cdn-v3`
  `{content_type,file_name}` -> `{file_url, upload_url}`, then `PUT` the bytes to `upload_url` (no key).
  **This upload endpoint is not in fal's documentation**; it was only confirmed to work live. If it fails the
  client falls back to the data URI and logs `upload failed (...)`.
- **The uploaded file is deleted after the job**: `POST https://rest.fal.ai/storage/files/delete`
  `{file_url}`; the key needs `assets:write`. A RAII `Cleanup` does it on every exit path and logs
  `removed the uploaded picture...` or `could NOT be removed...`.
- Errors: an invalid key is HTTP 401 `invalid key credentials`. **Qt hides the real HTTP error when the server
  closes the connection early**, so probe with a request of your own instead of trusting `Unable to write`.
  HTTP error texts all come from `httpErrorMessage`.
- Ask the user before probing with a key that is not yours, or with the key the user stored.

## 5. Settings and storage

- Settings live in `%LOCALAPPDATA%\kritarc` with the keys `aichat_base_url`, `aichat_api_key`, `aichat_model`,
  `aichat_send_reasoning_effort`, `aichat_vision_base_url`, `aichat_vision_api_key`, `aichat_vision_model`,
  `aichat_vision_disable_thinking`, `aichat_vision_provider_blacklist`, `aichat_fal_api_key`. They are read and
  written by `Config::load/save`; the settings page is Configure Krita -> "AI Chat" (`Preferences`, registered
  through `KisPreferenceSetRegistry`).
- **API keys are stored in plain text in kritarc** (like every other Krita setting), and the user knows. Never
  put a key into the log, into notes or into a test.
- Chat history: `%APPDATA%\krita\aichat\chat.sqlite` (`Store`). "New chat" keeps the old conversation in the
  database; the whole conversation is the request context (no truncation) and there is no list of old
  conversations yet.
- Log: `%APPDATA%\krita\aichat\aichat.log` (`writeLog`; cut to its last part above 1 MB). Krita started from
  the Start menu has no console, so the log is the only way to find out afterwards why something was slow.
  Vision requests log the provider (the `provider` field of OpenRouter). Slow ones were almost always the same
  provider; put its name (e.g. Nebius) into "Blocked providers" in the settings to make OpenRouter skip it.
  The provider of the main chat request is **not** logged yet.
- `reasoning_effort` is only sent when the setting says so (many compatible servers reject the field); the
  vision request asks the model not to think by default.

## 6. Pitfalls

- **Namespace + metatypes**: `Q_DECLARE_METATYPE` has to be at global scope and name `AiChat::X`. Queued calls
  by string (`invokeMethod("submit", Q_ARG(RequestSettings, ...))`) look the type up by its **short name**, so
  also register `qRegisterMetaType<T>("T")` (see `Client`, `Worker`, `Dock`). Do the same for every new type
  that crosses threads, otherwise Qt reports "unregistered datatype".
- `QT_USE_QSTRINGBUILDER` is on: the result of a string concatenation cannot be assigned to a `QJsonValue`
  directly; wrap it in `QString(...)`.
- The moc of Qt 5.15 skips files that contain raw string literals with escaped quotes.
- `Dock` does not need `setObjectName`: `KisMainWindow::createDockWidget` sets the object name to the `id()` of
  the factory (`"AiChat"`). The `id()` must be unique and must not change after a release, or the layouts users
  saved are lost.
- `KisNode::add` with a null `aboveThis` inserts at the bottom (index 0).
- Diagnostics of a clang-based IDE ("file not found" and the like) are noise: it has no Qt/Krita include paths.
  Trust the real build.
- While Krita runs, `kritaaichatdocker.dll` is locked: rename the loaded dll (e.g. into `krita-deps\old-dlls\`)
  and copy the new one; **it takes effect after Krita restarts**.

## 7. Build, test, deploy (Windows)

- Toolchain: llvm-mingw UCRT plus the prebuilt CI dependencies, in a separate `krita-deps` folder (called
  `<krita-deps>` below). In PowerShell:
  - add `toolchain\llvm-mingw-...\bin`, CMake, Ninja, `work\krita\_install\bin` and `_install\lib` to `PATH`;
  - set `QT_LOGGING_TO_CONSOLE=1` and `QT_FORCE_STDERR_LOGGING=1`;
  - `cd work\krita\_build; ninja; ctest -R aichat --output-on-failure`.
- The `No module named 'PyQt5'/'sipbuild'` that CMake prints when it reconfigures is an unrelated old problem and
  does not stop the build.
- Tests (8 programs): `DockTest`, `PreferencesTest`, `StoreTest`, `StreamTest`, `WorkerTest`, `ToolsTest`,
  `FalTest`, `LayerOpsTest`. Tests that touch settings must call
  `QStandardPaths::setTestModeEnabled(true)` so the real kritarc is never written.
- All network tests use `FakeOpenAiServer` (scripted responses); nothing talks to a real service.

## 8. Coding conventions

These come from Krita's own `HACKING` file (in the root of the Krita source tree); follow them in new code.

- **Class and file names**: in Krita's libraries every new class has the `Kis` prefix and the file has the same
  name as the class (`KisNewClass.h`, `KisNewClass.cpp`). **File names in plugins do not start with `Kis`.**
  Do not create new classes that start with `Ko`. Old files with `kis_snake_case.h` names are legacy; new files
  use CamelCase.
- **Design patterns**: when a well-known pattern is used, the class name says so (`...Factory`, `...Command`,
  `...Visitor`). Docker plugins in `plugins/dockers` are `XxxPlugin` + `XxxDockFactory` + `XxxDock`; ours are
  `AiChatPlugin`, `AiChatDockFactory` and `Dock`.
- **Functions**: camelCase, verb then noun (`rotateLayer`, not `layer_rotate`).
- **Variables and parameters**: start with a lower-case letter, camelCase, no abbreviations except very common
  ones (`cs` for a color space). Long, explicit names are fine.
- **Members**: only the `m_` prefix for class-level variables; no `g_`, `l_`, no `p` for pointers.
- **Getters and setters**: `x()` and `setX(int x)`. No Qt properties.
- **Pointers**: use smart pointers where possible, preferably Qt's. `QScopedPointer` only for a d-pointer in
  simple classes; otherwise `std::unique_ptr`.
- **Where this plugin differs on purpose**: its classes have no `Kis` prefix; they live in `namespace AiChat`
  instead, which keeps the names short and avoids symbol clashes with other plugins. If the plugin is ever
  merged into Krita itself, the classes must get the `Kis` prefix (file names stay as they are).

## 9. Not done / open

- No confirmation card before the paid Topaz tools and the destructive layer tools (`delete_layer`,
  `scale_image`); only the tool descriptions tell the AI to use them when the user asks.
- Tool calls are not stored in the database and have no card in the chat.
- The main chat path was never tried against a real OpenAI/DeepSeek server (only the scripted fake one). The
  official DeepSeek API takes no images, so the vision model has to come from elsewhere (e.g. Qwen-VL,
  Gemini).
- The "Add image" button only emits `addImageRequested()`.
- Models that edit part of a picture from a mask (inpainting, outpainting) are not connected: Topaz takes no mask.
- The provider of the main chat request is not logged, and OpenRouter routing is not configurable.
