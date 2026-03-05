/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * 语音对话系统 Demo (非 AEC 模式)
 *
 * 使用独立的 AudioCapture 和 AudioPlayer，适用于硬件自带 AEC 的场景
 * 录音默认 16kHz/1ch，直连 VAD/STT，无需重采样
 * 播放使用独立采样率（默认 48kHz）
 * 支持 barge-in（用户打断 TTS 播放）
 *
 * 用法:
 *   ./voice_chat [--tts matcha:zh|matcha:en|matcha:zh-en|kokoro|kokoro:<voice>] [--model qwen2.5:0.5b] [--input-device 0] [--output-device 0]
 */

#include <iostream>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <deque>
#include <sstream>
#include <functional>
#include <memory>
#include <queue>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <ctime>
#include <cmath>
#include <fstream>
#include <set>

// Audio capture/playback
#include "audio_base.hpp"

// Resampler (capture rate <-> 16kHz, TTS rate -> playback rate)
#include "audio_resampler.hpp"

// STT
#include "asr_service.h"

// TTS
#include "tts_service.h"

// VAD
#include "vad_service.h"

// LLM
#include "llm_service.h"

// 流式 TTS 分句
#include "text_buffer.hpp"

// MCP SDK (可选)
#ifdef USE_MCP
#include <curl/curl.h>
#include <map>
#include <mcp_api.hpp>
#endif

// ============================================================================
// 时间戳辅助函数
// ============================================================================

std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << "[" << std::setfill('0')
        << std::setw(2) << tm_now.tm_hour << ":"
        << std::setw(2) << tm_now.tm_min << ":"
        << std::setw(2) << tm_now.tm_sec << "."
        << std::setw(3) << ms.count() << "]";
    return oss.str();
}

// ============================================================================
// 全局状态
// ============================================================================

std::atomic<bool> g_running{true};
std::atomic<bool> g_processing{false};  // 正在处理 LLM/TTS
std::atomic<bool> g_barge_in{false};    // barge-in 触发标志

std::mutex g_process_thread_mutex;
std::unique_ptr<std::thread> g_process_thread;

void signalHandler(int sig) {
    (void)sig;
    std::cout << "\n" << getTimestamp() << " [退出中...]" << std::endl;
    g_running = false;
}

// ============================================================================
// TTS 引擎选择辅助
// ============================================================================

struct EngineSelection {
    SpacemiT::BackendType backend;
    std::string voice;  // Only used by Kokoro
};

static const std::vector<std::pair<std::string, std::string>> kKokoroVoices = {
    // Chinese female
    {"zf_xiaobei",  "xiaobei"},
    {"zf_xiaoni",   "xiaoni"},
    {"zf_xiaoxiao", "xiaoxiao"},
    {"zf_xiaoyi",   "xiaoyi"},
    // Chinese male
    {"zm_yunxi",    "yunxi"},
    {"zm_yunyang",  "yunyang"},
    {"zm_yunjian",  "yunjian"},
    {"zm_yunfan",   "yunfan"},
    // American English female
    {"af_heart",    "heart"},
    {"af_alloy",    "alloy"},
    {"af_aoede",    "aoede"},
    {"af_bella",    "bella"},
    {"af_jessica",  "jessica"},
    {"af_kore",     "kore"},
    {"af_nicole",   "nicole"},
    {"af_nova",     "nova"},
    {"af_river",    "river"},
    {"af_sarah",    "sarah"},
    {"af_sky",      "sky"},
    // American English male
    {"am_adam",     "adam"},
    {"am_echo",     "echo"},
    {"am_eric",     "eric"},
    {"am_fenrir",   "fenrir"},
    {"am_liam",     "liam"},
    {"am_michael",  "michael"},
    {"am_onyx",     "onyx"},
    {"am_puck",     "puck"},
    // British English female
    {"bf_alice",    "alice"},
    {"bf_emma",     "emma"},
    {"bf_isabella", "isabella"},
    {"bf_lily",     "lily"},
    // British English male
    {"bm_daniel",   "daniel"},
    {"bm_fable",    "fable"},
    {"bm_george",   "george"},
    {"bm_lewis",    "lewis"},
};

std::string resolveVoiceName(const std::string& input) {
    if (input.empty()) return input;

    if (input.find('_') != std::string::npos) {
        return input;
    }

    std::vector<std::string> matches;
    for (const auto& [full, shortname] : kKokoroVoices) {
        if (shortname == input) {
            matches.push_back(full);
        }
    }

    if (matches.size() == 1) {
        std::cout << "音色: " << input << " -> " << matches[0] << std::endl;
        return matches[0];
    }

    if (matches.size() > 1) {
        std::cerr << "错误: 音色名 '" << input << "' 有多个匹配:\n";
        for (const auto& m : matches) {
            std::cerr << "  " << m << "\n";
        }
        std::cerr << "请使用完整名称，如 --tts kokoro:" << matches[0] << "\n";
        exit(1);
    }

    std::cerr << "警告: 未知音色 '" << input << "'，将直接使用该名称\n"
        << "使用 --list-voices 查看可用音色列表\n";
    return input;
}

void printVoiceList() {
    std::cout << "Kokoro 可用音色列表:\n"
        << "\n"
        << "中文女声 (zf_):\n"
        << "  zf_xiaobei      小北 (默认)\n"
        << "  zf_xiaoni       小妮\n"
        << "  zf_xiaoxiao     小小\n"
        << "  zf_xiaoyi       小一\n"
        << "\n"
        << "中文男声 (zm_):\n"
        << "  zm_yunxi        云希\n"
        << "  zm_yunyang      云阳\n"
        << "  zm_yunjian      云健\n"
        << "  zm_yunfan       云帆\n"
        << "\n"
        << "美式英语女声 (af_):\n"
        << "  af_heart        Heart\n"
        << "  af_alloy        Alloy\n"
        << "  af_aoede        Aoede\n"
        << "  af_bella        Bella\n"
        << "  af_jessica      Jessica\n"
        << "  af_kore         Kore\n"
        << "  af_nicole       Nicole\n"
        << "  af_nova         Nova\n"
        << "  af_river        River\n"
        << "  af_sarah        Sarah\n"
        << "  af_sky          Sky\n"
        << "\n"
        << "美式英语男声 (am_):\n"
        << "  am_adam         Adam\n"
        << "  am_echo         Echo\n"
        << "  am_eric         Eric\n"
        << "  am_fenrir       Fenrir\n"
        << "  am_liam         Liam\n"
        << "  am_michael      Michael\n"
        << "  am_onyx         Onyx\n"
        << "  am_puck         Puck\n"
        << "\n"
        << "英式英语女声 (bf_):\n"
        << "  bf_alice        Alice\n"
        << "  bf_emma         Emma\n"
        << "  bf_isabella     Isabella\n"
        << "  bf_lily         Lily\n"
        << "\n"
        << "英式英语男声 (bm_):\n"
        << "  bm_daniel       Daniel\n"
        << "  bm_fable        Fable\n"
        << "  bm_george       George\n"
        << "  bm_lewis        Lewis\n"
        << "\n"
        << "用法: --tts kokoro:<voice>  支持短名 (xiaobei) 和全名 (zf_xiaobei)\n"
        << std::endl;
}

EngineSelection parseEngine(const std::string& spec) {
    EngineSelection sel;
    sel.backend = SpacemiT::BackendType::MATCHA_ZH;

    auto colon = spec.find(':');
    std::string engine = (colon != std::string::npos) ? spec.substr(0, colon) : spec;
    std::string variant = (colon != std::string::npos) ? spec.substr(colon + 1) : "";

    if (engine == "matcha") {
        if (variant.empty() || variant == "zh") {
            sel.backend = SpacemiT::BackendType::MATCHA_ZH;
        } else if (variant == "en") {
            sel.backend = SpacemiT::BackendType::MATCHA_EN;
        } else if (variant == "zh-en" || variant == "zhen") {
            sel.backend = SpacemiT::BackendType::MATCHA_ZH_EN;
        } else {
            std::cerr << "错误: 未知 Matcha 变体 '" << variant << "'\n"
                << "可用变体: zh, en, zh-en\n";
            exit(1);
        }
        return sel;
    }

    if (engine == "kokoro") {
        sel.backend = SpacemiT::BackendType::KOKORO;
        sel.voice = resolveVoiceName(variant);
        return sel;
    }

    std::cerr << "错误: 未知引擎 '" << engine << "'\n"
        << "可用引擎: matcha, kokoro\n"
        << "用法: --tts matcha:zh 或 --tts kokoro:zf_xiaobei\n";
    exit(1);
}

// ============================================================================
// 参数配置
// ============================================================================

struct Config {
    std::string tts_type = "matcha:zh";
    bool list_voices = false;
    std::string llm_model = "qwen2.5:0.5b";
    std::string llm_url = "";
    int input_device = -1;
    int output_device = -1;
    float vad_threshold = 0.8f;
    float silence_duration = 0.5f;
    int max_tokens = 150;
    bool list_devices = false;

    // Audio config (independent capture/playback)
    int capture_rate = 16000;
    int capture_channels = 1;
    int playback_rate = 48000;
    int playback_channels = 1;

    // 调试：音频录制
    bool save_audio = false;
    std::string audio_file = "voice_debug.wav";

    // MCP 配置
    std::string mcp_config_path = "";
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tts") == 0 && i + 1 < argc) {
            cfg.tts_type = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.llm_model = argv[++i];
        } else if ((strcmp(argv[i], "--llm-url") == 0 || strcmp(argv[i], "--llm_url") == 0) && i + 1 < argc) {
            cfg.llm_url = argv[++i];
        } else if ((strcmp(argv[i], "--input-device") == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) {
            cfg.input_device = std::stoi(argv[++i]);
        } else if ((strcmp(argv[i], "--output-device") == 0 || strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
            cfg.output_device = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--list-devices") == 0 || strcmp(argv[i], "-l") == 0) {
            cfg.list_devices = true;
        } else if (strcmp(argv[i], "--capture-rate") == 0 && i + 1 < argc) {
            cfg.capture_rate = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-channels") == 0 && i + 1 < argc) {
            cfg.capture_channels = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--playback-rate") == 0 && i + 1 < argc) {
            cfg.playback_rate = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--playback-channels") == 0 && i + 1 < argc) {
            cfg.playback_channels = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-audio") == 0) {
            cfg.save_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.audio_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--mcp-config") == 0 && i + 1 < argc) {
            cfg.mcp_config_path = argv[++i];
        } else if (strcmp(argv[i], "--list-voices") == 0) {
            cfg.list_voices = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                << "\n音频设备:\n"
                << "  -i, --input-device <id>       输入设备索引 (默认: 系统默认)\n"
                << "  -o, --output-device <id>      输出设备索引 (默认: 系统默认)\n"
                << "  -l, --list-devices            列出可用音频设备\n"
                << "\n音频参数:\n"
                << "  --capture-rate <hz>           录音采样率 (默认: 16000)\n"
                << "  --capture-channels <n>        录音声道数 (默认: 1)\n"
                << "  --playback-rate <hz>          播放采样率 (默认: 48000)\n"
                << "  --playback-channels <n>       播放声道数 (默认: 1)\n"
                << "\nLLM:\n"
                << "  --model <name>                LLM模型 (默认: qwen2.5:0.5b)\n"
                << "  --llm-url <url>               LLM API地址 (默认: 使用ollama)\n"
                << "\nTTS:\n"
                << "  --tts <engine>                TTS后端 (默认: matcha:zh)\n"
                << "                                matcha:zh / matcha:en / matcha:zh-en\n"
                << "                                kokoro / kokoro:<voice>\n"
                << "  --list-voices                 列出 Kokoro 可用音色\n"
                << "\n调试:\n"
                << "  --save-audio [file]           保存录音 (默认: voice_debug.wav)\n"
                << "\nMCP:\n"
                << "  --mcp-config <path>           MCP配置文件 (启用工具调用)\n"
                << "\n其他:\n"
                << "  -h, --help                    显示帮助\n";
            exit(0);
        }
    }
    return cfg;
}

// ============================================================================
// MCP 配置和 LLM 后端 (可选)
// ============================================================================

#ifdef USE_MCP

using json = nlohmann::json;

struct MCPConfig {
    std::string backend = "ollama";
    std::string url = "http://localhost:11434";
    std::string model = "qwen2.5:0.5b";
    int timeout = 120;
    std::string system_prompt = "你是一个智能助手，可以使用工具帮助用户。请用中文回复。";
    std::string registry_url = "";
    int registry_poll_interval = 5;

    struct ServerEntry {
        std::string name;
        std::string type;  // "stdio", "socket", "http"
        std::string command;
        std::vector<std::string> args;
        std::string socketPath;
        std::string url;
    };
    std::vector<ServerEntry> servers;
};

bool loadMCPConfig(const std::string& path, MCPConfig& config) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << getTimestamp() << " [MCP] 无法打开配置文件: " << path << std::endl;
        return false;
    }

    try {
        json j;
        f >> j;

        if (j.contains("backend")) config.backend = j["backend"];
        if (j.contains("url")) config.url = j["url"];
        if (j.contains("model")) config.model = j["model"];
        if (j.contains("timeout")) config.timeout = j["timeout"];
        if (j.contains("system_prompt")) config.system_prompt = j["system_prompt"];
        if (j.contains("registry_url")) config.registry_url = j["registry_url"];
        if (j.contains("registry_poll_interval")) config.registry_poll_interval = j["registry_poll_interval"];

        if (j.contains("servers")) {
            for (const auto& srv : j["servers"]) {
                MCPConfig::ServerEntry entry;
                entry.name = srv["name"];
                entry.type = srv.value("type", "http");

                if (entry.type == "stdio") {
                    entry.command = srv["command"];
                    if (srv.contains("args")) {
                        for (const auto& arg : srv["args"]) {
                            entry.args.push_back(arg);
                        }
                    }
                } else if (entry.type == "http") {
                    entry.url = srv["url"];
                } else {
                    entry.socketPath = srv.value("path", srv.value("socket", ""));
                }
                config.servers.push_back(entry);
            }
        }

        if (config.url == "http://localhost:11434" && config.backend == "llama") {
            config.url = "http://localhost:8080";
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << getTimestamp() << " [MCP] 配置解析错误: " << e.what() << std::endl;
        return false;
    }
}

std::string convertMCPToolsToString(const std::vector<mcp::Tool>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        json tool_json = t.toJson();
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool_json["name"]},
                {"description", tool_json["description"]},
                {"parameters", tool_json["inputSchema"]}
            }}
        });
    }
    return arr.dump();
}

std::vector<MCPConfig::ServerEntry> fetchServicesFromRegistry(const std::string& registry_url) {
    std::vector<MCPConfig::ServerEntry> services;
    if (registry_url.empty()) return services;

    CURL* curl = curl_easy_init();
    if (!curl) return services;

    std::string response_data;
    curl_easy_setopt(curl, CURLOPT_URL, registry_url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        (reinterpret_cast<std::string*>(userdata))->append(
            reinterpret_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return services;

    try {
        json j = json::parse(response_data);
        if (j.contains("services")) {
            for (const auto& srv : j["services"]) {
                MCPConfig::ServerEntry entry;
                entry.name = srv["name"];
                entry.type = srv.value("type", "http");
                entry.url = srv.value("url", "");
                services.push_back(entry);
            }
        }
    } catch (...) {}

    return services;
}

#endif  // USE_MCP

// ============================================================================
// 列出音频设备
// ============================================================================

void listAudioDevices() {
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "            可用音频设备\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    std::cout << getTimestamp() << " 输入设备 (麦克风):\n";
    auto input_devices = SpaceAudio::AudioCapture::ListDevices();
    if (input_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : input_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n输出设备 (扬声器):\n";
    auto output_devices = SpaceAudio::AudioPlayer::ListDevices();
    if (output_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : output_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n使用方法:\n";
    std::cout << getTimestamp() << "   voice_chat -i <输入设备ID> -o <输出设备ID>\n";
    std::cout << getTimestamp() << " ========================================\n";
}

// ============================================================================
// TTS 音频转换
// ============================================================================

std::vector<float> pcm16BytesToFloat(const std::vector<uint8_t>& bytes) {
    size_t num_samples = bytes.size() / 2;
    std::vector<float> output(num_samples);
    const int16_t* samples = reinterpret_cast<const int16_t*>(bytes.data());

    for (size_t i = 0; i < num_samples; ++i) {
        output[i] = samples[i] / 32768.0f;
    }

    return output;
}

// ============================================================================
// WAV 文件写入（用于调试）
// ============================================================================

void saveWav(const std::string& filename, const std::vector<int16_t>& data, int sample_rate) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "无法创建文件: " << filename << std::endl;
        return;
    }

    uint32_t data_size = static_cast<uint32_t>(data.size() * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;

    // RIFF header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&file_size), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;  // PCM
    uint16_t num_channels = 1;
    uint32_t sr = static_cast<uint32_t>(sample_rate);
    uint32_t byte_rate = sr * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    file.write(reinterpret_cast<const char*>(&fmt_size), 4);
    file.write(reinterpret_cast<const char*>(&audio_format), 2);
    file.write(reinterpret_cast<const char*>(&num_channels), 2);
    file.write(reinterpret_cast<const char*>(&sr), 4);
    file.write(reinterpret_cast<const char*>(&byte_rate), 4);
    file.write(reinterpret_cast<const char*>(&block_align), 2);
    file.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    // data chunk
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&data_size), 4);
    file.write(reinterpret_cast<const char*>(data.data()), data_size);
}

// ============================================================================
// 主程序
// ============================================================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);

    Config cfg = parseArgs(argc, argv);

    // 列出设备模式
    if (cfg.list_devices) {
        listAudioDevices();
        return 0;
    }

    // 列出 Kokoro 音色
    if (cfg.list_voices) {
        printVoiceList();
        return 0;
    }

    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "    语音对话系统 (非 AEC 模式)\n";
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << " TTS后端: " << cfg.tts_type << "\n";
    std::cout << getTimestamp() << " LLM模型: " << cfg.llm_model << "\n";
    if (!cfg.llm_url.empty()) {
        std::cout << getTimestamp() << " LLM URL: " << cfg.llm_url << "\n";
    } else {
        std::cout << getTimestamp() << " LLM后端: Ollama\n";
    }
    std::cout << getTimestamp() << " 录音: " << cfg.capture_rate << " Hz / "
        << cfg.capture_channels << " ch\n";
    std::cout << getTimestamp() << " 播放: " << cfg.playback_rate << " Hz / "
        << cfg.playback_channels << " ch\n";
    std::cout << getTimestamp() << " 按 Ctrl+C 退出\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    // -------------------------------------------------------------------------
    // 1. 初始化 LLM 引擎
    // -------------------------------------------------------------------------
    std::string llm_api_base;
    if (!cfg.llm_url.empty()) {
        llm_api_base = cfg.llm_url;
    } else {
        llm_api_base = "http://localhost:11434/v1";
    }
    std::string system_prompt = "You are a helpful assistant.";

    auto llm = std::make_shared<spacemit_llm::LLMService>(
        cfg.llm_model, llm_api_base, "EMPTY", system_prompt, cfg.max_tokens);

    if (cfg.llm_url.empty()) {
        std::cout << getTimestamp() << " [1/5] 检查 Ollama..." << std::flush;
        if (!llm->IsOllamaAvailable()) {
            std::cerr << "\n" << getTimestamp() << " 错误: Ollama 未运行，请先启动: ollama serve\n";
            return 1;
        }
        std::cout << " OK (v" << llm->GetOllamaVersion() << ")\n";

        auto models = llm->ListOllamaModels();
        bool model_found = false;
        for (const auto& m : models) {
            if (m.find(cfg.llm_model) != std::string::npos) {
                model_found = true;
                break;
            }
        }
        if (!model_found) {
            std::cout << getTimestamp() << "   下载模型 " << cfg.llm_model << "...\n";
            llm->PullOllamaModel(cfg.llm_model);
        }
    } else {
        std::cout << getTimestamp() << " [1/5] LLM 后端: " << cfg.llm_url << " OK\n";
    }

    // -------------------------------------------------------------------------
    // 2. 初始化 VAD
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [2/5] 初始化 VAD..." << std::flush;

    auto vad_config = SpacemiT::VadConfig::Preset("silero")
        .withTriggerThreshold(cfg.vad_threshold)
        .withStopThreshold(cfg.vad_threshold - 0.15f);

    auto vad = std::make_shared<SpacemiT::VadEngine>(vad_config);
    if (!vad->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: VAD 初始化失败\n";
        return 1;
    }
    std::cout << " OK (" << vad->GetEngineName() << ")\n";

    // -------------------------------------------------------------------------
    // 3. 初始化 ASR
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [3/5] 初始化 ASR..." << std::flush;
    auto asr = std::make_shared<SpacemiT::AsrEngine>();
    if (!asr->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: ASR 初始化失败\n";
        return 1;
    }
    std::cout << " OK\n";

    // -------------------------------------------------------------------------
    // 4. 初始化 TTS
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [4/5] 初始化 TTS (" << cfg.tts_type << ")..." << std::flush;

    auto selection = parseEngine(cfg.tts_type);

    SpacemiT::TtsConfig tts_cfg;
    tts_cfg.backend = selection.backend;

    if (selection.backend == SpacemiT::BackendType::KOKORO && !selection.voice.empty()) {
        tts_cfg.voice = selection.voice;
    }

    int tts_sample_rate;
    switch (selection.backend) {
        case SpacemiT::BackendType::MATCHA_ZH:
        case SpacemiT::BackendType::MATCHA_EN:
            tts_sample_rate = 22050;
            break;
        case SpacemiT::BackendType::MATCHA_ZH_EN:
            tts_sample_rate = 16000;
            break;
        case SpacemiT::BackendType::KOKORO:
            tts_sample_rate = 24000;
            break;
        default:
            tts_sample_rate = 22050;
    }
    tts_cfg.sample_rate = tts_sample_rate;

    auto tts = std::make_shared<SpacemiT::TtsEngine>(tts_cfg);
    if (!tts->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: TTS 初始化失败\n";
        return 1;
    }
    std::cout << " OK\n";

    // -------------------------------------------------------------------------
    // 5. 初始化音频设备
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [5/5] 初始化音频设备..." << std::flush;

    SpaceAudio::AudioCapture capture(cfg.input_device);
    SpaceAudio::AudioPlayer player(cfg.output_device);

    // 初始化录音重采样器（capture_rate != 16kHz 时使用）
    std::unique_ptr<Resampler> capture_resampler;
    if (cfg.capture_rate != 16000) {
        Resampler::Config rconf;
        rconf.input_sample_rate = cfg.capture_rate;
        rconf.output_sample_rate = 16000;
        rconf.channels = 1;  // Mix to mono first, then resample
        rconf.method = (cfg.capture_rate > 16000)
            ? ResampleMethod::LINEAR_DOWNSAMPLE
            : ResampleMethod::LINEAR_UPSAMPLE;
        capture_resampler = std::make_unique<Resampler>(rconf);
        if (!capture_resampler->initialize()) {
            std::cerr << "\n" << getTimestamp() << " 错误: 录音重采样器初始化失败\n";
            return 1;
        }
    }

    // 初始化播放重采样器（TTS 采样率 != 播放采样率时使用）
    std::unique_ptr<Resampler> playback_resampler;
    if (tts_sample_rate != cfg.playback_rate) {
        Resampler::Config rconf;
        rconf.input_sample_rate = tts_sample_rate;
        rconf.output_sample_rate = cfg.playback_rate;
        rconf.channels = 1;  // TTS output is mono
        rconf.method = (tts_sample_rate < cfg.playback_rate)
            ? ResampleMethod::LINEAR_UPSAMPLE
            : ResampleMethod::LINEAR_DOWNSAMPLE;
        playback_resampler = std::make_unique<Resampler>(rconf);
        if (!playback_resampler->initialize()) {
            std::cerr << "\n" << getTimestamp() << " 错误: 播放重采样器初始化失败\n";
            return 1;
        }
    }

    std::cout << " OK\n";
    if (cfg.capture_rate == 16000 && cfg.capture_channels == 1) {
        std::cout << getTimestamp() << " 录音管道: 16kHz/1ch -> 直连 VAD/STT (零重采样)\n";
    } else {
        std::cout << getTimestamp() << " 录音管道: " << cfg.capture_rate << "Hz/"
            << cfg.capture_channels << "ch -> ";
        if (cfg.capture_channels > 1) std::cout << "混音->mono -> ";
        if (cfg.capture_rate != 16000) std::cout << "重采样->16kHz -> ";
        std::cout << "VAD/STT\n";
    }
    if (tts_sample_rate == cfg.playback_rate) {
        std::cout << getTimestamp() << " 播放管道: TTS(" << tts_sample_rate << "Hz) -> 直连播放\n";
    } else {
        std::cout << getTimestamp() << " 播放管道: TTS(" << tts_sample_rate << "Hz) -> 重采样->"
            << cfg.playback_rate << "Hz -> 播放\n";
    }
    std::cout << "\n";

    // -------------------------------------------------------------------------
    // 6. 初始化 MCP (可选)
    // -------------------------------------------------------------------------
#ifdef USE_MCP
    MCPConfig mcp_cfg;
    bool mcp_enabled = false;
    std::unique_ptr<mcp::MCPManager> mcp_manager;
    std::string llm_tools_json;
    std::vector<spacemit_llm::ChatMessage> conversation_messages;
    std::thread registry_poll_thread;
    std::mutex tools_mutex;
    std::set<std::string> known_servers;

    if (!cfg.mcp_config_path.empty()) {
        std::cout << getTimestamp() << " [MCP] 加载配置: " << cfg.mcp_config_path << "\n";

        if (loadMCPConfig(cfg.mcp_config_path, mcp_cfg)) {
            mcp_enabled = true;

            llm->update_model(mcp_cfg.model);
            llm->update_prompt(mcp_cfg.system_prompt);
            system_prompt = mcp_cfg.system_prompt;
            if (!mcp_cfg.url.empty()) {
                std::string api_base = mcp_cfg.url;
                if (api_base.back() == '/') api_base.pop_back();
                if (api_base.size() < 3 || api_base.substr(api_base.size() - 3) != "/v1") {
                    api_base += "/v1";
                }
                llm->update_api_settings(api_base, "EMPTY");
            }
            std::cout << getTimestamp() << " [MCP] LLM后端: " << mcp_cfg.url << "\n";
            std::cout << getTimestamp() << " [MCP] 模型: " << mcp_cfg.model << "\n";

            mcp_manager = std::make_unique<mcp::MCPManager>();

            for (const auto& srv : mcp_cfg.servers) {
                known_servers.insert(srv.name);
                if (srv.type == "http") {
                    mcp::HttpConfig hc;
                    hc.url = srv.url;
                    mcp_manager->addHttpServer(srv.name, hc);
                    std::cout << getTimestamp()
                        << " [MCP] 添加服务器: " << srv.name
                        << " (http: " << srv.url << ")\n";
                } else if (srv.type == "stdio") {
                    mcp::StdioConfig sc;
                    sc.command = srv.command;
                    sc.args = srv.args;
                    mcp_manager->addStdioServer(srv.name, sc);
                    std::cout << getTimestamp()
                        << " [MCP] 添加服务器: " << srv.name
                        << " (stdio: " << srv.command << ")\n";
                } else if (srv.type == "socket") {
                    mcp::UnixSocketConfig uc;
                    uc.socketPath = srv.socketPath;
                    mcp_manager->addUnixSocketServer(srv.name, uc);
                    std::cout << getTimestamp()
                        << " [MCP] 添加服务器: " << srv.name
                        << " (socket: " << srv.socketPath << ")\n";
                }
            }

            if (!mcp_cfg.registry_url.empty()) {
                std::cout << getTimestamp() << " [MCP] 从注册中心获取服务: " << mcp_cfg.registry_url << "\n";
                auto registry_services = fetchServicesFromRegistry(mcp_cfg.registry_url);
                for (const auto& srv : registry_services) {
                    if (known_servers.find(srv.name) == known_servers.end()) {
                        known_servers.insert(srv.name);
                        mcp::HttpConfig hc;
                        hc.url = srv.url;
                        mcp_manager->addHttpServer(srv.name, hc);
                        std::cout << getTimestamp()
                            << " [MCP] 添加服务器: " << srv.name
                            << " (http: " << srv.url << ")\n";
                    }
                }
            }

            std::cout << getTimestamp() << " [MCP] 启动服务器...\n";
            mcp_manager->startAll();

            if (mcp_manager->waitForAnyServer(std::chrono::milliseconds(10000))) {
                auto tools = mcp_manager->getAllTools();
                llm_tools_json = convertMCPToolsToString(tools);
                std::cout << getTimestamp() << " [MCP] 已连接 "
                    << mcp_manager->readyServerCount()
                    << " 个服务器, " << tools.size() << " 个工具\n";
            } else {
                std::cout << getTimestamp() << " [MCP] 警告: 无可用服务器，继续等待...\n";
            }

            conversation_messages.push_back(spacemit_llm::ChatMessage::System(mcp_cfg.system_prompt));

            if (!mcp_cfg.registry_url.empty()) {
                std::cout << getTimestamp() << " [MCP] 启动注册中心轮询: " << mcp_cfg.registry_url << "\n";
                registry_poll_thread = std::thread([&]() {
                    while (g_running) {
                        auto services = fetchServicesFromRegistry(mcp_cfg.registry_url);

                        std::set<std::string> registry_services;
                        std::map<std::string, std::string> service_urls;
                        for (const auto& srv : services) {
                            registry_services.insert(srv.name);
                            service_urls[srv.name] = srv.url;
                        }

                        std::vector<std::string> to_remove;
                        for (const auto& name : known_servers) {
                            auto status = mcp_manager->getStatus(name);

                            if (registry_services.find(name) == registry_services.end()) {
                                if (status.state == mcp::ServerState::Error ||
                                    status.state == mcp::ServerState::Disconnected) {
                                    to_remove.push_back(name);
                                    std::cout << "\n" << getTimestamp() << " [MCP] 服务已下线: " << name << "\n";
                                }
                            } else if (status.state == mcp::ServerState::Error ||
                                status.state == mcp::ServerState::Disconnected) {
                                std::cout << "\n" << getTimestamp() << " [MCP] 尝试重连: " << name << "\n";
                                mcp_manager->startServer(name);
                            }
                        }

                        for (const auto& name : to_remove) {
                            mcp_manager->removeServer(name);
                            known_servers.erase(name);
                        }

                        bool new_services_added = false;
                        for (const auto& srv : services) {
                            if (known_servers.find(srv.name) == known_servers.end()) {
                                mcp::HttpConfig hc;
                                hc.url = srv.url;
                                mcp_manager->addHttpServer(srv.name, hc);
                                mcp_manager->startServer(srv.name);
                                known_servers.insert(srv.name);
                                new_services_added = true;
                                std::cout << "\n" << getTimestamp()
                                    << " [MCP] 发现新服务: " << srv.name
                                    << " (" << srv.url << ")\n";
                            }
                        }

                        if (!to_remove.empty() || new_services_added) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            {
                                std::lock_guard<std::mutex> lock(tools_mutex);
                                auto tools = mcp_manager->getAllTools();
                                llm_tools_json = convertMCPToolsToString(tools);
                                std::cout << getTimestamp() << " [MCP] 工具列表已更新: " << tools.size() << " 个工具\n";
                            }
                        }

                        std::this_thread::sleep_for(std::chrono::seconds(mcp_cfg.registry_poll_interval));
                    }
                });
            }

            std::cout << getTimestamp() << " [MCP] 初始化完成\n\n";
        } else {
            std::cout << getTimestamp() << " [MCP] 配置加载失败，使用默认 LLM\n\n";
        }
    }
#endif  // USE_MCP

    // -------------------------------------------------------------------------
    // 播放队列和播放线程
    // -------------------------------------------------------------------------
    std::queue<std::vector<uint8_t>> playback_queue;
    std::mutex playback_mutex;
    std::condition_variable playback_cv;
    std::atomic<bool> is_playing{false};

    // 启动播放设备
    if (!player.Start(cfg.playback_rate, cfg.playback_channels)) {
        std::cerr << getTimestamp() << " 错误: 无法启动播放设备\n";
        return 1;
    }

    // 播放线程：从队列取数据写入 AudioPlayer，队列空时写静音防止 ALSA XRUN
    std::thread playback_thread([&]() {
        // 静音 chunk：20ms @ playback_rate, playback_channels, PCM16
        const size_t silence_frames = cfg.playback_rate / 50;  // 20ms
        const size_t silence_bytes = silence_frames * cfg.playback_channels * sizeof(int16_t);
        const std::vector<uint8_t> silence(silence_bytes, 0);

        while (g_running) {
            std::vector<uint8_t> chunk;
            {
                std::unique_lock<std::mutex> lock(playback_mutex);
                playback_cv.wait_for(lock, std::chrono::milliseconds(20),
                                     [&] { return !playback_queue.empty() || !g_running; });
                if (!g_running) break;
                if (!playback_queue.empty()) {
                    chunk = std::move(playback_queue.front());
                    playback_queue.pop();
                }
            }
            if (chunk.empty()) {
                // 队列空，写静音保活防止 XRUN
                player.Write(silence);
                is_playing = false;
            } else {
                is_playing = true;
                player.Write(chunk);
                {
                    std::lock_guard<std::mutex> lock(playback_mutex);
                    if (playback_queue.empty()) {
                        is_playing = false;
                    }
                }
            }
        }
    });

    // 入队播放数据：float mono -> resample -> expand channels -> PCM16 bytes -> enqueue
    auto enqueuePlayback = [&](const std::vector<float>& float_samples, int src_rate) {
        // Resample to playback rate if needed
        std::vector<float> resampled;
        if (src_rate != cfg.playback_rate && playback_resampler) {
            resampled = playback_resampler->process(float_samples);
        } else {
            resampled = float_samples;
        }

        // Expand mono to N channels if needed
        size_t total_samples;
        if (cfg.playback_channels > 1) {
            total_samples = resampled.size() * cfg.playback_channels;
        } else {
            total_samples = resampled.size();
        }

        // Convert float to PCM16 bytes (with optional channel expansion)
        std::vector<uint8_t> pcm_bytes(total_samples * 2);
        int16_t* out = reinterpret_cast<int16_t*>(pcm_bytes.data());

        if (cfg.playback_channels > 1) {
            for (size_t i = 0; i < resampled.size(); ++i) {
                int16_t sample = static_cast<int16_t>(
                    std::clamp(resampled[i], -1.0f, 1.0f) * 32767.0f);
                for (int ch = 0; ch < cfg.playback_channels; ++ch) {
                    out[i * cfg.playback_channels + ch] = sample;
                }
            }
        } else {
            for (size_t i = 0; i < resampled.size(); ++i) {
                out[i] = static_cast<int16_t>(
                    std::clamp(resampled[i], -1.0f, 1.0f) * 32767.0f);
            }
        }

        {
            // 分成 20ms 小 chunk 入队，确保 barge-in 能快速中断播放
            const size_t chunk_bytes =
                (cfg.playback_rate / 50) * cfg.playback_channels * sizeof(int16_t);
            std::lock_guard<std::mutex> lock(playback_mutex);
            for (size_t offset = 0; offset < pcm_bytes.size(); offset += chunk_bytes) {
                size_t len = std::min(chunk_bytes, pcm_bytes.size() - offset);
                playback_queue.push(
                    std::vector<uint8_t>(pcm_bytes.data() + offset,
                                         pcm_bytes.data() + offset + len));
            }
        }
        playback_cv.notify_one();
    };

    // 清空播放队列（用于 barge-in）
    auto clearPlayback = [&]() {
        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            std::queue<std::vector<uint8_t>> empty;
            playback_queue.swap(empty);
        }
        is_playing = false;
        // stream 保持运行，播放线程会自动切换到写静音
    };

    // -------------------------------------------------------------------------
    // 状态变量
    // -------------------------------------------------------------------------
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames = 0;
    const int silence_frames_threshold =
        static_cast<int>(cfg.silence_duration * 16000 / 512);  // 512 samples per VAD frame
    bool is_speaking = false;
    int frame_count = 0;

    // 预缓冲区（非 AEC 模式增大预缓冲，避免 barge-in 丢帧）
    const size_t PRE_BUFFER_FRAMES = 30;
    std::deque<std::vector<float>> pre_buffer;

    // Barge-in 连续帧检测（非 AEC 模式适当放宽阈值，避免假阳性）
    int barge_in_confirm_frames = 0;
    const int BARGE_IN_CONFIRM_THRESHOLD = 5;

    // Barge-in 录音状态
    std::atomic<bool> barge_in_recording{false};

    // 音频录制缓冲区（用于调试）
    std::vector<int16_t> recorded_audio;
    std::mutex record_mutex;

    // VAD 帧累积缓冲区（512 samples = 32ms @ 16kHz，Silero VAD 推荐帧大小）
    const size_t VAD_FRAME_SIZE = 512;
    std::vector<float> vad_frame_buffer;

    // -------------------------------------------------------------------------
    // 处理识别结果 -> LLM -> TTS (流式：边生成边播放)
    // -------------------------------------------------------------------------
    auto processText = [&](const std::string& text) {
        if (text.empty()) return;

        g_processing = true;
        g_barge_in = false;

        std::cout << "\n" << getTimestamp() << " [你]: " << text << "\n";

        TextBuffer text_buffer;
        std::string full_response;
        int sentence_count = 0;

        // 流式 TTS 合成函数：每完成一句立即合成并入队播放
        auto synthesizeSentence = [&](const std::string& sentence) {
            if (sentence.empty() || g_barge_in || !g_running) return;

            sentence_count++;
            auto result = tts->Call(sentence);
            if (result && result->IsSuccess() && !g_barge_in) {
                auto audio_bytes = result->GetAudioData();
                if (!audio_bytes.empty()) {
                    auto float_samples = pcm16BytesToFloat(audio_bytes);
                    enqueuePlayback(float_samples, tts_sample_rate);
                }
            }
        };

#ifdef USE_MCP
        // MCP 模式：支持工具调用
        if (mcp_enabled) {
            conversation_messages.push_back(spacemit_llm::ChatMessage::User(text));

            const int MAX_TOOL_ROUNDS = 10;
            int round = 0;

            while (round++ < MAX_TOOL_ROUNDS && g_running && !g_barge_in) {
                std::cout << getTimestamp() << " [LLM] 第 " << round << " 轮...\n";
                std::cout << getTimestamp() << " [AI]: " << std::flush;

                std::string current_tools;
                {
                    std::lock_guard<std::mutex> lock(tools_mutex);
                    current_tools = llm_tools_json;
                }

                auto result = llm->chat_stream(
                    conversation_messages,
                    [&](const std::string& chunk, bool is_done, const std::string& error) -> bool {
                        if (g_barge_in || !g_running) return false;
                        if (!error.empty()) {
                            std::cerr << "\n" << getTimestamp() << " [LLM错误] " << error << std::endl;
                            return false;
                        }
                        if (is_done) return true;

                        if (!chunk.empty()) {
                            std::cout << chunk << std::flush;
                            full_response += chunk;
                            text_buffer.addText(chunk);

                            while (text_buffer.hasSentence() && !g_barge_in) {
                                std::string sentence = text_buffer.getNextSentence();
                                synthesizeSentence(sentence);
                            }
                        }
                        return true;
                    },
                    current_tools);

                std::cout << std::endl;

                if (result.HasToolCalls()) {
                    std::cout << getTimestamp() << " [Tool Call] 检测到工具调用\n";

                    conversation_messages.push_back(
                        spacemit_llm::ChatMessage::Assistant(result.content, result.tool_calls_json));

                    try {
                        auto tool_calls = json::parse(result.tool_calls_json);
                        for (const auto& tc : tool_calls) {
                            std::string tool_name = tc["function"]["name"];
                            json tool_args = tc["function"]["arguments"];

                            if (tool_args.is_string()) {
                                try {
                                    tool_args = json::parse(tool_args.get<std::string>());
                                } catch (...) {}
                            }

                            std::string server = mcp_manager->findServerForTool(tool_name);
                            std::cout << getTimestamp() << " [MCP] 调用: " << tool_name
                                << " @ " << server << " 参数: "
                                << tool_args.dump() << std::endl;

                            auto tool_result = mcp_manager->callTool(tool_name, tool_args);

                            std::string result_text;
                            if (tool_result.success && !tool_result.contents.empty()) {
                                result_text = tool_result.contents[0];
                            } else if (!tool_result.error.empty()) {
                                result_text = "错误: " + tool_result.error;
                            } else {
                                result_text = tool_result.rawResult.dump();
                            }

                            std::cout << getTimestamp() << " [MCP] 结果: " << result_text << std::endl;

                            std::string tc_id = tc.value("id", "");
                            conversation_messages.push_back(
                                spacemit_llm::ChatMessage::Tool(result_text, tc_id));
                        }
                    } catch (const std::exception& e) {
                        std::cerr << getTimestamp() << " [MCP] 工具调用解析错误: " << e.what() << std::endl;
                    }

                    full_response.clear();
                    text_buffer.clear();
                    continue;
                }

                conversation_messages.push_back(
                    spacemit_llm::ChatMessage::Assistant(result.content));

                if (!g_barge_in) {
                    text_buffer.stop();
                    std::string remaining = text_buffer.getNextSentence();
                    if (!remaining.empty()) {
                        synthesizeSentence(remaining);
                    }
                }
                break;
            }
        } else {
#else
        {
#endif  // USE_MCP
            // 非 MCP 模式
            std::cout << getTimestamp() << " [LLM] 开始生成...\n";
            std::cout << getTimestamp() << " [AI]: " << std::flush;

            std::vector<spacemit_llm::ChatMessage> msgs;
            msgs.push_back(spacemit_llm::ChatMessage::System(system_prompt));
            msgs.push_back(spacemit_llm::ChatMessage::User(text));

            auto result = llm->chat_stream(msgs,
                [&](const std::string& chunk, bool is_done, const std::string& error) -> bool {
                    if (g_barge_in || !g_running) return false;
                    if (!error.empty()) {
                        std::cerr << "\n" << getTimestamp() << " [LLM错误] " << error << std::endl;
                        return false;
                    }
                    if (is_done) return true;

                    if (!chunk.empty()) {
                        std::cout << chunk << std::flush;
                        full_response += chunk;
                        text_buffer.addText(chunk);

                        while (text_buffer.hasSentence() && !g_barge_in) {
                            std::string sentence = text_buffer.getNextSentence();
                            synthesizeSentence(sentence);
                        }
                    }
                    return true;
                });

            if (result.cancelled && g_barge_in) {
                std::cout << "\n" << getTimestamp() << " [LLM] 已因 barge-in 中断生成\n";
            } else if (!result.error.empty()) {
                std::cerr << "\n" << getTimestamp() << " [LLM错误] " << result.error << std::endl;
                g_processing = false;
                return;
            }

            std::cout << std::endl;

            if (!g_barge_in) {
                text_buffer.stop();
                std::string remaining = text_buffer.getNextSentence();
                if (!remaining.empty()) {
                    synthesizeSentence(remaining);
                }
            }
        }

        if (sentence_count > 0) {
            std::cout << getTimestamp() << " [TTS] 流式合成完成 (" << sentence_count << " 句)\n";
        }

        // 等待播放完成
        while (is_playing.load() && g_running && !g_barge_in) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // 清理缓冲区
        if (!g_barge_in) {
            {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                audio_buffer.clear();
                pre_buffer.clear();
                silence_frames = 0;
                is_speaking = false;
            }
            barge_in_recording = false;
            vad_frame_buffer.clear();
            vad->Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << getTimestamp() << " [TTS] 播放完成，缓冲区已清理\n";
        } else {
            std::cout << getTimestamp() << " [TTS] Barge-in 打断，保留音频缓冲区\n";
            g_barge_in = false;
        }

        g_processing = false;
        std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;
    };  // NOLINT(readability/braces)

    // -------------------------------------------------------------------------
    // 设置录音回调
    // -------------------------------------------------------------------------
    capture.SetCallback([&](const uint8_t* data, size_t size) {
        if (!g_running) return;

        // PCM16 little-endian -> float
        size_t num_samples = size / 2;
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
        std::vector<float> float_samples(num_samples);
        for (size_t i = 0; i < num_samples; ++i) {
            float_samples[i] = pcm[i] / 32768.0f;
        }

        // 多声道混音到 mono
        if (cfg.capture_channels > 1) {
            size_t frames = num_samples / cfg.capture_channels;
            std::vector<float> mono(frames);
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0.0f;
                for (int ch = 0; ch < cfg.capture_channels; ++ch) {
                    sum += float_samples[i * cfg.capture_channels + ch];
                }
                mono[i] = sum / cfg.capture_channels;
            }
            float_samples = std::move(mono);
        }

        // 重采样到 16kHz（如果需要）
        std::vector<float> samples_16k;
        if (capture_resampler) {
            samples_16k = capture_resampler->process(float_samples);
        } else {
            samples_16k = std::move(float_samples);
        }

        if (samples_16k.empty()) return;

        // 录制音频（用于调试）
        if (cfg.save_audio) {
            std::lock_guard<std::mutex> lock(record_mutex);
            for (float s : samples_16k) {
                recorded_audio.push_back(static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
            }
        }

        // 累积音频到 VAD 帧缓冲区
        vad_frame_buffer.insert(vad_frame_buffer.end(), samples_16k.begin(), samples_16k.end());

        // 当缓冲区达到 VAD 帧大小时进行检测
        while (vad_frame_buffer.size() >= VAD_FRAME_SIZE && g_running) {
            // 取出一帧
            std::vector<float> vad_frame(vad_frame_buffer.begin(), vad_frame_buffer.begin() + VAD_FRAME_SIZE);
            vad_frame_buffer.erase(vad_frame_buffer.begin(), vad_frame_buffer.begin() + VAD_FRAME_SIZE);

            // VAD 检测
            auto vad_result = vad->Detect(vad_frame);
            float vad_prob = vad_result ? vad_result->GetProbability() : 0.0f;

            // 每10帧打印一次 VAD 状态（仅在非处理期间）
            frame_count++;
            if (frame_count % 10 == 0 && !g_processing) {
                std::cout << "\r" << getTimestamp() << " [VAD] prob=" << std::fixed
                    << std::setprecision(2) << vad_prob
                    << " speaking=" << (is_speaking ? "Y" : "N")
                    << " buffer=" << audio_buffer.size()
                    << " playing=" << (is_playing.load() ? "Y" : "N")
                    << "      " << std::flush;
            }

            // TTS 播放期间：检测 barge-in
            if (g_processing) {
                // Barge-in 录音模式：持续累积音频
                if (barge_in_recording && is_speaking) {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());

                    if (vad_prob <= cfg.vad_threshold) {
                        silence_frames++;
                    } else {
                        silence_frames = 0;
                    }
                    continue;
                }

                // Barge-in 检测（需要连续多帧确认）
                if (is_playing.load() && vad_prob > cfg.vad_threshold) {
                    barge_in_confirm_frames++;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES + BARGE_IN_CONFIRM_THRESHOLD) {
                        pre_buffer.pop_front();
                    }

                    if (barge_in_confirm_frames >= BARGE_IN_CONFIRM_THRESHOLD) {
                        std::cout << "\n" << getTimestamp() << " [Barge-in] 用户打断 (连续"
                            << barge_in_confirm_frames << "帧, prob=" << vad_prob
                            << ")，停止播放\n";
                        clearPlayback();
                        g_barge_in = true;
                        barge_in_recording = true;
                        barge_in_confirm_frames = 0;

                        // Barge-in 后立即开始累积用户语音
                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        is_speaking = true;
                        audio_buffer.clear();
                        for (const auto& frame : pre_buffer) {
                            audio_buffer.insert(audio_buffer.end(), frame.begin(), frame.end());
                        }
                        pre_buffer.clear();
                        silence_frames = 0;
                    }
                } else {
                    barge_in_confirm_frames = 0;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES) {
                        pre_buffer.pop_front();
                    }
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(buffer_mutex);

            if (vad_prob > cfg.vad_threshold) {
                if (!is_speaking) {
                    is_speaking = true;
                    audio_buffer.clear();

                    for (const auto& frame : pre_buffer) {
                        audio_buffer.insert(audio_buffer.end(), frame.begin(), frame.end());
                    }
                    pre_buffer.clear();

                    std::cout << "\n" << getTimestamp() << " [VAD] 开始说话 (prob=" << vad_prob << ")...\n";
                }
                audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                silence_frames = 0;
            } else if (is_speaking) {
                audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                silence_frames++;

                if (silence_frames >= silence_frames_threshold) {
                    is_speaking = false;
                    barge_in_recording = false;
                    std::cout << "\n" << getTimestamp() << " [VAD] 停止说话，触发识别\n";

                    if (audio_buffer.size() > 8000) {
                        std::cout << getTimestamp() << " [ASR] 开始识别...\n";
                        auto result = asr->Recognize(audio_buffer, 16000);
                        if (result && !result->IsEmpty()) {
                            std::string text = result->GetText();
                            std::cout << getTimestamp() << " [ASR] 识别完成: \"" << text << "\"\n";
                            {
                                std::lock_guard<std::mutex> lock2(g_process_thread_mutex);
                                if (g_process_thread && g_process_thread->joinable()) {
                                    g_process_thread->join();
                                }
                                g_process_thread = std::make_unique<std::thread>([&processText, text]() {
                                    processText(text);
                                });
                            }
                        } else {
                            std::cout << getTimestamp() << " [ASR] 识别完成: (无结果)\n";
                        }
                    }

                    audio_buffer.clear();
                    silence_frames = 0;
                }
            } else {
                pre_buffer.push_back(vad_frame);
                if (pre_buffer.size() > PRE_BUFFER_FRAMES) {
                    pre_buffer.pop_front();
                }
            }
        }
    });

    // -------------------------------------------------------------------------
    // 开始对话
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;

    if (!capture.Start(cfg.capture_rate, cfg.capture_channels)) {
        std::cerr << getTimestamp() << " 错误: 无法启动录音设备\n";
        player.Stop();
        player.Close();
        return 1;
    }

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 等待 processText 线程完成
    {
        std::lock_guard<std::mutex> lock(g_process_thread_mutex);
        if (g_process_thread && g_process_thread->joinable()) {
            g_process_thread->join();
        }
    }

    // 清理音频设备
    capture.Stop();
    capture.Close();

    player.Stop();
    playback_cv.notify_all();
    if (playback_thread.joinable()) {
        playback_thread.join();
    }
    player.Close();

#ifdef USE_MCP
    if (mcp_enabled) {
        if (registry_poll_thread.joinable()) {
            registry_poll_thread.join();
        }
        if (mcp_manager) {
            mcp_manager->stopAll();
        }
        std::cout << getTimestamp() << " [MCP] 已清理\n";
    }
#endif

    // 保存录制的音频
    if (cfg.save_audio && !recorded_audio.empty()) {
        std::cout << getTimestamp() << " [保存音频] " << cfg.audio_file
            << " (" << recorded_audio.size() << " samples, "
            << (recorded_audio.size() / 16000.0f) << " 秒)\n";
        saveWav(cfg.audio_file, recorded_audio, 16000);
    }

    std::cout << "\n" << getTimestamp() << " [已退出]\n";
    return 0;
}
