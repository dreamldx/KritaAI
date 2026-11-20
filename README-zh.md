<h1>KritaAI <i>— Krita 的 AI 对话面板</i></h1>

[English](README.md) | **中文**

✨[功能](#features) | 🛠️[开始使用](#getting-started) | ⚙️[设置](#configure) | 🔒[隐私](#privacy) | 🧩[开发](#development) | 🐞[问题反馈](https://github.com/dreamldx/KritaAI/issues)

这是 [Krita](https://krita.org) 的 C++ 插件,在 Krita 里增加一个 **AI Chat** 面板。你可以和任何
OpenAI 兼容的模型对话,模型会通过工具查看你的文档、修改图层和选区,还能通过 [fal.ai](https://fal.ai)
把图片交给 Topaz 放大或降噪。

这个项目的主要目标:
* **能动手的聊天。** AI 不只是一个文字框。它能读取打开的文档,向视觉模型询问东西在哪里,选中它们并编辑图层。
  你只要说想要什么,不用再到菜单里找。
* **没有无法撤销的改动。** AI 做的每一次修改(图层、选区、Topaz 结果)都是 Krita 自己撤销历史里的一步。
  Topaz 的结果总是放进新图层,不会覆盖你原来的像素。
* **你的服务器,你的 key。** 任何 OpenAI 兼容的服务器都行:官方 API、OpenRouter 或本地服务器。
  消息只会发给你设置的服务器,不会连接别的地方。

## <a name="features"></a> 功能

* **聊天面板**:消息卡片有图标、名字、Markdown 正文,底部显示用掉的 token 数和 AI 花的秒数。
* **任何 OpenAI 兼容的服务器**:`POST {服务器}/chat/completions`,支持流式输出(SSE)、工具调用和可选的 `reasoning_effort`。
* **Markdown 回答**:标题、列表、表格、代码、引用、链接。回答里的原始 HTML 只当文字显示,只有 `http`、`https` 和 `mailto` 链接可以打开。
* **不卡界面**:AI 循环、网络和数据库都在工作线程里,Krita 不会因为等 AI 而卡住。
* **历史记录**:聊天记录保存在 SQLite,下次启动还在。
* **文档工具**:`get_document_info`(尺寸、分辨率、色彩模型、含 id 和混合模式的图层、选区)和 `ask_about_image`,后者把画布缩小到最长边 1024 像素以内,发给*视觉模型*。被问到某样东西在哪里时,视觉模型用 0 到 1 的比例给出框 `[left, top, right, bottom]`。
* **选区**:`box_select` 用 0 到 1 的比例选择一个矩形(大于 1 的数会被拒绝)。
* **图层工具**:`add_layer`、`add_group`、`rename_layer`、`set_layer_visibility`、`set_layer_opacity`、`set_layer_blend_mode`、`move_layer`、`duplicate_layer`、`delete_layer`、`reorder_layer`、`move_layer_to_group`、`scale_image`。
* **通过 fal.ai 使用 Topaz**:`upscale_image` 和 `denoise_image`。图片(或选中的区域)先上传再处理,结果放进一个**新图层**。任务运行时会显示带取消按钮的模态进度窗口,结束后会把上传到 fal.ai 的文件删掉。
* **撤销**:上面这些修改每一次都是 *编辑 → 撤销* 里的一步。
* **日志文件**:记录每次请求的耗时和 token 数,用来查为什么慢。

工具只在能用的时候才提供:`ask_about_image` 需要视觉模型,Topaz 工具需要 fal.ai key。

## <a name="getting-started"></a> 开始使用

### 环境要求

<table>
<tr><td>Krita</td><td>一份能编译通过的 Krita 源码(master 分支);这是 C++ 插件,要和 Krita 一起编译</td></tr>
<tr><td>Qt</td><td>5.15(Core、Gui、Widgets、Network、Sql 以及 SQLite 驱动)</td></tr>
<tr><td>平台</td><td>开发环境是 Windows,用的是 Krita 官方 CI 的依赖和 llvm-mingw UCRT 工具链;代码本身没有 Windows 专有的东西</td></tr>
<tr><td>AI 服务器</td><td>任何 OpenAI 兼容的对话服务器;可选一个视觉模型和一个 fal.ai key</td></tr>
</table>

### 安装

目前没有现成的下载:C++ 插件必须和加载它的 Krita 匹配。仓库的目录结构和 Krita 源码一致,可以直接复制到 Krita 源码树里:

1. 把插件复制进 Krita 源码:
   ```bash
   # 在本仓库的根目录
   cp -r plugins/dockers/aichat  /path/to/krita/plugins/dockers/
   ```
2. 在 Krita 的 `plugins/dockers/CMakeLists.txt` 里加一行:
   ```cmake
   add_subdirectory(aichat)
   ```
3. 像平常一样配置并编译 Krita。目标名是 `kritaaichatdocker`,会和其他 Krita 插件一起安装。
4. 启动 Krita,打开面板:*设置 → 工具面板 → AI Chat*。
5. 在 *设置 → 配置 Krita → AI Chat* 里填写服务器(见[设置](#configure))。

> [!NOTE]
> C++ 插件只在 Krita 启动时加载一次,所以每次重新编译后都要重启 Krita。

### _可选:_ 视觉模型

在设置里填一个视觉模型,AI 就能看图(`ask_about_image`)。它会用比例回答东西在哪里,AI 再把这些比例交给
`box_select`。视觉模型可以和聊天模型在不同的服务器上。

### _可选:_ Topaz 放大和降噪

填一个 [fal.ai](https://fal.ai) key。这个 key 需要 `assets:write` 权限,这样上传的图片才能在任务后被删除。

## <a name="configure"></a> 设置

*设置 → 配置 Krita → AI Chat*

| 项目 | 含义 |
|---|---|
| Server address(服务器地址) | 到 `/v1` 为止的地址,例如 `https://api.openai.com/v1` 或 `https://openrouter.ai/api/v1`。程序会自己加上 `/chat/completions`。 |
| API key | 有些本地服务器不需要。以**明文**保存在 Krita 的 `kritarc` 里。 |
| Model(模型) | 模型名,例如 `gpt-4o-mini`。 |
| Send the thinking effort | 把面板上的 *Thinking* 选项作为 `reasoning_effort` 发出。如果你的服务器拒绝这个字段,就保持关闭。 |
| Vision model(地址、key、模型) | 可选。能读图片的模型,给 `ask_about_image` 用。可以和聊天模型在不同的服务器上。 |
| Ask the vision model not to think | 发送 `"reasoning": {"enabled": false}`(OpenRouter 的写法)。默认打开,服务器不接受时请关掉。 |
| Blocked providers(屏蔽的服务商) | 只对 OpenRouter 有效:不允许处理视觉请求的公司名(用逗号分隔),以 `provider.ignore` 发送。某个服务商很慢时有用。 |
| fal.ai key | Topaz 工具需要。 |

数据存放位置,Windows 上是 `%APPDATA%\krita\aichat\`(其他系统在 Krita 的数据目录里):

* `chat.sqlite`:聊天记录。
* `aichat.log`:每次请求一行,含耗时、token 数和处理请求的服务商。API key 和图片都不会写进日志。

设置保存在 Krita 的 `kritarc` 里(`aichat_*` 开头的键)。

## <a name="privacy"></a> 隐私

* 消息只会发给你设置的服务器,不会连接别的地方。
* 设置了视觉模型后,AI 一旦决定"看一眼",就会把**你打开的文档缩小后的图片**发给那台服务器。如果这不能接受,请不要设置视觉模型。
* 设置了 fal.ai key 后,Topaz 工具会把**图片(或选中的区域)**上传到 fal.ai 的文件存储,任务结束后删除。它们只在你要求时才会使用,而且要花钱。
* API key 以明文保存在 `kritarc` 里。

## <a name="status"></a> 当前状态

已完成:聊天、流式输出、Markdown、历史记录、设置、文档和视觉工具、图层和选区工具、通过 fal.ai 的 Topaz 放大和降噪、每次修改都能撤销。

尚未完成:

* 付费的 Topaz 工具和有破坏性的图层工具(`delete_layer`、`scale_image`)执行前没有确认卡片,目前只靠工具描述告诉 AI 要等用户要求才使用。
* 工具调用没有存进数据库,聊天里也没有对应的卡片。
* 根据遮罩修改图片局部的模型(局部重绘、扩图):Topaz 不接受遮罩。
* *Add image* 按钮目前只会发出一个信号。
* 旧对话列表:旧对话保存在数据库里,但界面只显示最新的一个。
* 超长对话的截断:每次请求都会发送整段对话。

## <a name="development"></a> 开发

欢迎贡献。请先读 [`plugins/dockers/aichat/AGENTS.md`](plugins/dockers/aichat/AGENTS.md)(英文):它说明了设计、线程规则、注意事项和编码规范(遵循 Krita 的 HACKING)。

所有源码都在 `plugins/dockers/aichat/` 里,属于 `namespace AiChat`。

| 目录 / 文件 | 作用 |
|---|---|
| `AiChat.*` | 插件本身:注册 docker 和设置页。 |
| `ai/Agent.*` | AI 循环:请求模型、运行它要求的工具、再次请求(最多 10 轮)。 |
| `ai/tools/` | 工具层。`Tools.*` 是工具注册表,以及在界面线程里安全执行函数的执行器;`DocumentTools.*`(`get_document_info`、`ask_about_image`)、`LayerOps.*` / `LayerTools.*`(图层、选区、撤销)、`Fal.*` / `FalTools.*`(通过 fal.ai 调用 Topaz)、`Vision.*`(给视觉模型的请求)。 |
| `network/` | `Client.*`:一次流式 HTTP 请求;`Stream.*`:SSE 解析,收集文字、工具调用和 token 用量。 |
| `config/Config.*` | 保存在 `kritarc` 里的设置。 |
| `model/` | 聊天的状态,不含任何控件:`Chat.*`(历史是否加载好、是否在回答、思考强度、已收到的文字;`send`、`stop`、`newChat`;拥有工作线程和工具)和 `Request.*`(把设置变成一次请求)。 |
| `ui/` | 视图:`Dock.*`(面板:显示一个 `Chat`;消息卡片、Markdown、输入框)、`MessageWidget.h`(卡片和按钮)、`Preferences.*`(*配置 Krita* 里的页面)、`Progress.*`(Topaz 工具的模态进度窗口)。 |
| `utils/` | `Worker.*`(运行在工作线程里,拥有 AI 循环和数据库)、`Store.*`(带结构迁移的 SQLite)、`Log.*`(日志文件)。 |

添加一个工具,就是往注册表里加一个 `Tool`(名字、描述、JSON schema、处理函数)。处理函数在工作线程里运行,只能通过 `GuiExecutor::run` 访问 Krita 的图像。

### 测试

测试不需要网络,也不需要真实的 AI 服务:它们使用测试自带的一个脚本化的小型 HTTP 服务器。

```bash
ctest -R aichat --output-on-failure
```

一共有九个测试程序:聊天模型、面板、设置、数据库、流解析、工作线程、工具、fal.ai 客户端和图层操作(图层测试使用真实的 Krita 图像和它的撤销历史)。

## 技术

* 宿主程序:[Krita](https://krita.org)(C++、Qt 5.15)
* 对话后端:任何 OpenAI 兼容的 [chat completions](https://platform.openai.com/docs/api-reference/chat) 接口,用 SSE 流式输出
* 视觉:任何接受图片的 OpenAI 兼容模型
* 放大和降噪:由 [fal.ai](https://fal.ai) 提供的 [Topaz](https://www.topazlabs.com) 模型
* 存储:通过 Qt 使用 SQLite

## 许可证

GPL-3.0-or-later,和 Krita 相同。见 [LICENSE](LICENSE)。
