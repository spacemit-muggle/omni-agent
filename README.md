# Omni Agent — 语音对话系统

SpacemiT 语音对话系统，集成 VAD + ASR + LLM + TTS + MCP 工具调用的端到端语音交互方案。

| 特性 | 说明 |
|------|------|
| 语音识别 | SenseVoice (16kHz) |
| 语音合成 | Matcha-TTS / Kokoro |
| 语音活动检测 | Silero VAD |
| LLM | OpenAI 兼容 API (llama.cpp) |
| 工具调用 | MCP (Model Context Protocol) |
| 回声消除 | WebRTC AEC3 (可选) |

## 快速开始

### 依赖

**组件依赖**（由构建系统自动解析）：

| 组件 | 说明 |
|------|------|
| `audio` | 音频采集/播放 + 重采样 |
| `asr` | 语音识别 (SenseVoice) |
| `tts` | 语音合成 (Matcha-TTS / Kokoro) |
| `vad` | 语音活动检测 (Silero) |
| `llm` | LLM 推理服务 |
| `mcp` | MCP 客户端库 |

**系统依赖**：

| 包名 | 用途 |
|------|------|
| `portaudio19-dev` | 音频 I/O |
| `libsndfile1-dev` | 音频文件读写 |
| `libfftw3-dev` | FFT 运算 |
| `libcurl4-openssl-dev` | HTTP 请求 |
| `meson` + `ninja-build` | WebRTC AEC 构建（USE_AEC=ON 时需要） |

### 构建

```bash
# 环境初始化
source build/envsetup.sh
lunch k3-com260-minimal    # 或其他目标

# 一键编译（构建 omni_agent 及其所有依赖）
m

# 标准构建（不含 AEC）
cd application/native/omni_agent && mm

# 启用 AEC 构建
cd application/native/omni_agent && mm -DUSE_AEC=ON

# 禁用 MCP
cd application/native/omni_agent && mm -DUSE_MCP=OFF
```

| CMake 选项 | 默认值 | 说明 |
|-----------|--------|------|
| `USE_MCP` | `ON` | MCP 工具调用支持 |
| `USE_AEC` | `OFF` | WebRTC 回声消除（编译 `voice_chat_aec`） |

### 运行

> LLM 服务启动方式参见 [llm 组件](<!-- TODO: 补充 llm 组件仓库链接 -->)。

```bash
# 最简运行
voice_chat -i 0 -o 0 --llm-url http://localhost:8080

# 指定模型和 TTS
voice_chat --llm-url http://localhost:8080 --model qwen2.5:7b --tts matcha:zn-en

# 启用 MCP 工具调用（MCP 服务开发和启动参见 [mcp 组件](<!-- TODO: 补充 mcp 组件仓库链接 -->)）
# 安装依赖（建议开虚拟环境）
pip install mcp starlette uvicorn psutil
# 启动注册中心
./components/smart_voice/mcp/examples/start_all_services.sh
voice_chat --llm-url http://localhost:8080 --mcp-config ./components/smart_voice/mcp/examples/configs/config_registry.json
```

## 两种模式

### voice_chat — 标准模式

适用于硬件自带 AEC 或外接麦克风的场景。录音和播放使用独立的音频设备，录音默认 16kHz/1ch 直连 VAD/STT（零重采样），播放使用独立采样率。

**参数列表**：

| 参数 | 缩写 | 默认值 | 说明 |
|------|------|--------|------|
| `--llm-url <url>` | | *必填* | LLM API 地址 |
| `--model <name>` | | `qwen2.5:0.5b` | LLM 模型名称 |
| `--tts <engine>` | | `matcha:zh` | TTS 后端（见下方说明） |
| `--input-device <id>` | `-i` | 系统默认 | 输入设备索引 |
| `--output-device <id>` | `-o` | 系统默认 | 输出设备索引 |
| `--list-devices` | `-l` | | 列出可用音频设备 |
| `--capture-rate <hz>` | | `16000` | 录音采样率 |
| `--capture-channels <n>` | | `1` | 录音声道数 |
| `--playback-rate <hz>` | | `48000` | 播放采样率 |
| `--playback-channels <n>` | | `1` | 播放声道数 |
| `--mcp-config <path>` | | | MCP 配置文件路径 |
| `--list-voices` | | | 列出 Kokoro 可用音色 |
| `--save-audio [file]` | | | 保存录音（默认 `voice_debug.wav`） |

**TTS 后端选项**：`matcha:zh` / `matcha:en` / `matcha:zh-en` / `kokoro` / `kokoro:<voice>`

### voice_chat_aec — AEC 模式 (USE_AEC=ON)

适用于需要软件回声消除的场景。使用全双工音频（48kHz），通过 WebRTC AEC3 实现回声消除，内部重采样到 16kHz 送 VAD/ASR。

**额外/不同参数**：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--sample-rate <hz>` | `48000` | 全双工音频采样率 |
| `--no-aec` | | 禁用回声消除 |
| `--no-ns` | | 禁用噪声抑制 |
| `--agc` | | 启用自动增益控制（默认禁用） |
| `--aec-delay <ms>` | `50` | AEC 延迟补偿（范围 20-100ms） |
| `--buffer-frames <n>` | `480` | 音频缓冲帧数 |

> `voice_chat_aec` 不支持 `--capture-rate`/`--playback-rate`，统一使用 `--sample-rate`。

## Barge-in（用户打断）

两种模式均支持用户打断正在播放的 TTS 回复：

1. TTS 播放期间，VAD 持续检测麦克风输入
2. 当检测到连续多帧语音活动（标准模式 5 帧 / AEC 模式 3 帧），判定为用户打断
3. 立即停止 TTS 播放，中断 LLM 生成
4. 保留已捕获的语音缓冲，开始新一轮识别

## MCP 工具集成

### 概述

omni_agent 通过 [MCP (Model Context Protocol)](https://modelcontextprotocol.io/) 支持 LLM 工具调用。运行时，omni_agent 作为 MCP 客户端连接外部工具服务，LLM 可以根据用户意图自动选择并调用工具，将结果融入对话。

### 配置文件格式

```json
{
  "backend": "llama",
  "url": "http://localhost:8080",
  "model": "qwen2.5:7b",
  "timeout": 120,
  "system_prompt": "你是一个智能助手，可以使用工具帮助用户。请用中文回复。",
  "servers": [...],
  "registry_url": "http://127.0.0.1:9000/mcp/services",
  "registry_poll_interval": 5
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `backend` | 否 | LLM 后端类型（`llama` / `openai`） |
| `url` | 是 | LLM API 地址（覆盖命令行 `--llm-url`） |
| `model` | 是 | 模型名称（覆盖命令行 `--model`） |
| `timeout` | 否 | 请求超时秒数，默认 120 |
| `system_prompt` | 否 | 系统提示词（覆盖默认值） |
| `servers` | 是 | MCP 服务列表 |
| `registry_url` | 否 | 服务注册中心 URL |
| `registry_poll_interval` | 否 | 注册中心轮询间隔（秒），默认 5 |

### 服务连接方式

支持三种 MCP 服务传输方式：

**HTTP（Streamable HTTP）**：

```json
{
  "name": "Calculator",
  "type": "http",
  "url": "http://localhost:8001/mcp"
}
```

**stdio（标准输入/输出）**：

```json
{
  "name": "Calculator",
  "type": "stdio",
  "command": "python3",
  "args": ["examples/services/calculator/stdio_server.py"]
}
```

**Unix Socket**：

```json
{
  "name": "SystemMonitor",
  "type": "socket",
  "path": "/tmp/mcp_system_monitor.sock"
}
```

### 服务注册中心

配置 `registry_url` 后，omni_agent 会按 `registry_poll_interval` 秒轮询注册中心，自动发现新上线的 MCP 服务并连接，也会自动移除已下线的服务。适用于动态环境中服务数量不固定的场景。

```json
{
  "url": "http://localhost:8080",
  "model": "qwen2.5:7b",
  "registry_url": "http://127.0.0.1:9000/mcp/services",
  "registry_poll_interval": 5,
  "servers": []
}
```

### 完整示例

以下配置同时使用 stdio 和 socket 两种服务：

```json
{
  "backend": "llama",
  "url": "http://localhost:8080",
  "model": "qwen2.5:7b",
  "timeout": 120,
  "system_prompt": "你是一个智能助手，可以使用工具帮助用户。请用中文回复。",
  "servers": [
    {
      "name": "Calculator",
      "type": "stdio",
      "command": "python3",
      "args": ["examples/services/calculator/stdio_server.py"]
    },
    {
      "name": "SystemMonitor",
      "type": "socket",
      "path": "/tmp/mcp_system_monitor.sock"
    }
  ]
}
```

运行：

```bash
voice_chat --llm-url http://localhost:8080 --model qwen2.5:7b --mcp-config config.json
```

### 编写 MCP 服务

MCP 服务可以用任何语言实现。详细的开发指南和示例参见 [mcp 组件](<!-- TODO: 补充 mcp 组件仓库链接 -->)。

Python 示例服务：

- `services/calculator/` — 计算器 (stdio_server.py / socket_server.py / http_server.py)
- `services/time/` — 时间查询 (stdio_server.py / socket_server.py / http_server.py)
- `services/system_monitor/` — 系统监控 (http_server.py)

C 语言 MCP 服务（用于嵌入式设备）参见 `components/smart_voice/mlink/device/`，通过 `mlink_tool_create()` 注册工具。

## 架构

```
voice_chat / voice_chat_aec
    ├── AudioCapture ──→ [Resampler] ──→ VAD ──→ ASR
    │   (16kHz/48kHz)     (→16kHz)     (Silero)  (SenseVoice)
    │
    ├── LLM (OpenAI 兼容 API)
    │   └── MCP 工具调用（可选）
    │
    └── TTS ──→ [Resampler] ──→ AudioPlayer
        (Matcha/Kokoro)  (→48kHz)
```

- **voice_chat**: AudioCapture + AudioPlayer 独立运行，录音直连 VAD/STT
- **voice_chat_aec**: AecDuplexProcessor 全双工处理，WebRTC AEC3 消除回声后送 VAD/STT

## 常见问题

**Q: 找不到音频设备？**

运行 `voice_chat -l` 列出所有可用设备，然后用 `-i <id> -o <id>` 指定。

**Q: 模型文件在哪里？**

ONNX 模型文件由各组件（stt/tts/vad）自动管理，首次运行时从预设路径加载。确保已正确构建并安装对应组件。

**Q: 软回采 AEC 效果不好，如何调优？**

- 调整 `--aec-delay`：增大延迟补偿值（默认 50ms）可改善远端回声消除
- 确认 `--sample-rate` 与硬件实际采样率匹配
- 使用 `--save-audio` 录制 AEC 处理后的音频进行分析
- 在安静环境下先用 `--no-aec` 确认基本流程正常

**Q: LLM 响应慢？**

- 使用更小的模型（如 `qwen2.5:0.5b`）
- 检查网络延迟（`--llm-url` 指向的服务是否可达）

## License

Apache-2.0
