/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * 带 AEC 的语音对话系统 Demo (全双工模式)
 *
 * 基于 main_voice_chat.cpp，使用 AecDuplexProcessor 实现回声消除
 * 支持 barge-in（用户打断 TTS 播放）
 *
 * 用法:
 *   ./voice_chat_aec [--tts matcha:zh|matcha:en|matcha:zh-en|kokoro|kokoro:<voice>] [--model qwen2.5:0.5b] [--input-device 0] [--output-device 0] [--sample-rate 48000]
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
#include <functional>
#include <memory>
#include <vector>
#include <cmath>

// AEC 处理器
#include "aec_duplex_processor.hpp"

// 全双工音频（用于列出设备）
#include "audio_duplex.hpp"

// Shared modules
#include "voice_common.hpp"
#include "engine_init.hpp"
#include "voice_pipeline.hpp"

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

    // AEC 配置
    bool aec_enabled = true;
    bool ns_enabled = true;
    bool agc_enabled = false;
    int aec_delay_ms = 50;
    int buffer_frames = 0;
    int sample_rate = 48000;

    // 调试：音频录制
    bool save_audio = false;
    std::string audio_file = "aec_debug.wav";

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
        } else if (strcmp(argv[i], "--no-aec") == 0) {
            cfg.aec_enabled = false;
        } else if (strcmp(argv[i], "--no-ns") == 0) {
            cfg.ns_enabled = false;
        } else if (strcmp(argv[i], "--agc") == 0) {
            cfg.agc_enabled = true;
        } else if (strcmp(argv[i], "--aec-delay") == 0 && i + 1 < argc) {
            cfg.aec_delay_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--buffer-frames") == 0 && i + 1 < argc) {
            cfg.buffer_frames = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg.sample_rate = std::stoi(argv[++i]);
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
                << "  -i, --input-device <id>   输入设备索引 (默认: 系统默认)\n"
                << "  -o, --output-device <id>  输出设备索引 (默认: 系统默认)\n"
                << "  -l, --list-devices        列出可用音频设备\n"
                << "\nLLM:\n"
                << "  --model <name>            LLM模型 (默认: qwen2.5:0.5b)\n"
                << "  --llm-url <url>           LLM API地址 (必填)\n"
                << "\nTTS:\n"
                << "  --tts <engine>            TTS后端 (默认: matcha:zh)\n"
                << "                            matcha:zh / matcha:en / matcha:zh-en\n"
                << "                            kokoro / kokoro:<voice>\n"
                << "  --list-voices             列出 Kokoro 可用音色\n"
                << "\nAEC:\n"
                << "  --no-aec                  禁用回声消除\n"
                << "  --no-ns                   禁用噪声抑制\n"
                << "  --agc                     启用自动增益控制 (默认禁用)\n"
                << "  --aec-delay <ms>          AEC延迟补偿 (默认: 50ms, 范围: 20-100ms)\n"
                << "  --buffer-frames <n>       音频缓冲帧数 (默认: 480)\n"
                << "  --sample-rate <hz>        音频采样率 (默认: 48000, 常用: 44100, 48000)\n"
                << "\n调试:\n"
                << "  --save-audio [file]       保存AEC处理后的音频 (默认: aec_debug.wav)\n"
                << "\nMCP:\n"
                << "  --mcp-config <path>       MCP配置文件 (启用工具调用)\n"
                << "\n其他:\n"
                << "  -h, --help                显示帮助\n";
            exit(0);
        }
    }
    return cfg;
}

// ============================================================================
// 列出音频设备
// ============================================================================

void listAudioDevices() {
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "            可用音频设备\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    std::cout << getTimestamp() << " 输入设备 (麦克风):\n";
    auto input_devices = SpaceAudio::AudioDuplex::ListInputDevices();
    if (input_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : input_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n输出设备 (扬声器):\n";
    auto output_devices = SpaceAudio::AudioDuplex::ListOutputDevices();
    if (output_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : output_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n使用方法:\n";
    std::cout << getTimestamp() << "   voice_chat_aec -i <输入设备ID> -o <输出设备ID>\n";
    std::cout << getTimestamp() << " ========================================\n";
}

// ============================================================================
// 重采样工具
// ============================================================================

std::vector<float> resampleToVad(const float* data, size_t frames, int from_rate) {
    if (from_rate == 16000) {
        return std::vector<float>(data, data + frames);
    }

    const double ratio = static_cast<double>(from_rate) / 16000.0;
    size_t output_frames = static_cast<size_t>(frames / ratio);
    std::vector<float> output(output_frames);

    for (size_t i = 0; i < output_frames; ++i) {
        double src_pos = i * ratio;
        size_t idx = static_cast<size_t>(src_pos);
        double frac = src_pos - idx;

        if (idx + 1 < frames) {
            output[i] = static_cast<float>(
                data[idx] * (1.0 - frac) + data[idx + 1] * frac);
        } else if (idx < frames) {
            output[i] = data[idx];
        } else {
            output[i] = 0.0f;
        }
    }

    return output;
}

std::vector<float> resampleToAec(const std::vector<float>& input, int from_rate, int to_rate) {
    if (from_rate == to_rate) return input;

    double ratio = static_cast<double>(to_rate) / from_rate;
    size_t output_size = static_cast<size_t>(input.size() * ratio);
    std::vector<float> output(output_size);

    for (size_t i = 0; i < output_size; ++i) {
        double src_pos = i / ratio;
        size_t src_idx = static_cast<size_t>(src_pos);
        double frac = src_pos - src_idx;

        if (src_idx + 1 < input.size()) {
            output[i] = static_cast<float>(
                input[src_idx] * (1.0 - frac) + input[src_idx + 1] * frac);
        } else if (src_idx < input.size()) {
            output[i] = input[src_idx];
        }
    }

    return output;
}

// ============================================================================
// 主程序
// ============================================================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);

    Config cfg = parseArgs(argc, argv);

    if (cfg.list_devices) {
        listAudioDevices();
        return 0;
    }

    if (cfg.list_voices) {
        printVoiceList();
        return 0;
    }

    if (cfg.llm_url.empty()) {
        std::cerr << "错误: 必须通过 --llm-url 指定 LLM API 地址\n";
        return 1;
    }

    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "    带 AEC 的语音对话系统 (全双工模式)\n";
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << " TTS后端: " << cfg.tts_type << "\n";
    std::cout << getTimestamp() << " LLM模型: " << cfg.llm_model << "\n";
    std::cout << getTimestamp() << " LLM URL: " << cfg.llm_url << "\n";
    std::cout << getTimestamp() << " AEC: " << (cfg.aec_enabled ? "ON" : "OFF") << "\n";
    std::cout << getTimestamp() << " AEC延迟补偿: " << cfg.aec_delay_ms << " ms\n";
    std::cout << getTimestamp() << " 噪声抑制: " << (cfg.ns_enabled ? "ON" : "OFF") << "\n";
    std::cout << getTimestamp() << " AGC: " << (cfg.agc_enabled ? "ON" : "OFF") << "\n";
    std::cout << getTimestamp() << " 采样率: " << cfg.sample_rate << " Hz (AEC) -> 16000 Hz (VAD/ASR)\n";
    std::cout << getTimestamp() << " 按 Ctrl+C 退出\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    // -------------------------------------------------------------------------
    // 1-4. 初始化引擎
    // -------------------------------------------------------------------------
    auto llm_result = initLLM(cfg.llm_model, cfg.llm_url, "You are a helpful assistant.", cfg.max_tokens);
    if (!llm_result.llm) return 1;
    auto llm = llm_result.llm;
    auto system_prompt = llm_result.system_prompt;

    auto vad = initVAD(cfg.vad_threshold);
    if (!vad) return 1;

    auto asr = initASR();
    if (!asr) return 1;

    auto tts_result = initTTS(cfg.tts_type);
    if (!tts_result.tts) return 1;
    auto tts = tts_result.tts;
    int tts_sample_rate = tts_result.sample_rate;

    // -------------------------------------------------------------------------
    // 5. 初始化 AEC 处理器
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [5/5] 初始化 AEC 音频处理器..." << std::flush;

    AecDuplexProcessor::Config aec_cfg;
    aec_cfg.sample_rate = cfg.sample_rate;
    aec_cfg.channels = 1;
    if (cfg.buffer_frames > 0) {
        aec_cfg.frames_per_buffer = cfg.buffer_frames;
    }
    aec_cfg.input_device = cfg.input_device;
    aec_cfg.output_device = cfg.output_device;
    aec_cfg.aec_enabled = cfg.aec_enabled;
    aec_cfg.ns_enabled = cfg.ns_enabled;
    aec_cfg.agc_enabled = cfg.agc_enabled;
    aec_cfg.estimated_delay_ms = cfg.aec_delay_ms;

    AecDuplexProcessor aec_processor(aec_cfg);
    if (!aec_processor.initialize()) {
        std::cerr << "\n" << getTimestamp() << " 错误: AEC 初始化失败\n";
        return 1;
    }
    std::cout << " OK\n\n";

    // -------------------------------------------------------------------------
    // 6. 初始化 MCP (可选)
    // -------------------------------------------------------------------------
#ifdef USE_MCP
    MCPInitResult mcp;
    initMCP(cfg.mcp_config_path, llm, system_prompt, mcp);
#endif

    // -------------------------------------------------------------------------
    // 状态变量
    // -------------------------------------------------------------------------
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames_count = 0;
    const int silence_frames_threshold =
        static_cast<int>(cfg.silence_duration * 16000 / 512);
    bool is_speaking = false;
    int frame_count = 0;

    const size_t PRE_BUFFER_FRAMES = 20;
    std::deque<std::vector<float>> pre_buffer;

    int barge_in_confirm_frames = 0;
    const int BARGE_IN_CONFIRM_THRESHOLD = 3;

    int post_barge_in_cooldown = 0;
    const int COOLDOWN_FRAMES = 15;

    std::atomic<bool> barge_in_recording{false};

    std::vector<int16_t> recorded_audio;
    std::mutex record_mutex;

    const size_t VAD_FRAME_SIZE = 512;
    std::vector<float> vad_frame_buffer;

    // -------------------------------------------------------------------------
    // 构造 VoicePipelineContext
    // -------------------------------------------------------------------------
    VoicePipelineContext pipeline_ctx;
    pipeline_ctx.llm = llm;
    pipeline_ctx.tts = tts;
    pipeline_ctx.vad = vad;
    pipeline_ctx.tts_sample_rate = tts_sample_rate;
    pipeline_ctx.system_prompt = system_prompt;
    pipeline_ctx.enqueue_playback = [&](const std::vector<float>& samples, int rate) {
        auto audio_aec = resampleToAec(samples, rate, cfg.sample_rate);
        aec_processor.enqueuePlayback(audio_aec, cfg.sample_rate);
    };
    pipeline_ctx.is_playing = [&]() { return aec_processor.isPlaying(); };
    pipeline_ctx.clear_playback = [&]() { aec_processor.clearPlayback(); };
    pipeline_ctx.audio_buffer = &audio_buffer;
    pipeline_ctx.buffer_mutex = &buffer_mutex;
    pipeline_ctx.silence_frames = &silence_frames_count;
    pipeline_ctx.is_speaking = &is_speaking;
    pipeline_ctx.barge_in_recording = &barge_in_recording;
    pipeline_ctx.vad_frame_buffer = &vad_frame_buffer;
    pipeline_ctx.pre_buffer = &pre_buffer;
#ifdef USE_MCP
    pipeline_ctx.mcp_manager = mcp.manager.get();
    pipeline_ctx.llm_tools_json = &mcp.llm_tools_json;
    pipeline_ctx.tools_mutex = &mcp.tools_mutex;
    pipeline_ctx.conversation_messages = &mcp.conversation_messages;
    pipeline_ctx.mcp_enabled = mcp.enabled;
#endif

    // -------------------------------------------------------------------------
    // 设置 AEC 处理器回调
    // -------------------------------------------------------------------------
    aec_processor.setAudioCallback([&](const float* data, size_t frames, int /*sample_rate*/) {
        if (!g_running) return;

        auto samples_16k = resampleToVad(data, frames, cfg.sample_rate);
        if (samples_16k.empty()) return;

        if (cfg.save_audio) {
            std::lock_guard<std::mutex> lock(record_mutex);
            for (float s : samples_16k) {
                recorded_audio.push_back(static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
            }
        }

        vad_frame_buffer.insert(vad_frame_buffer.end(), samples_16k.begin(), samples_16k.end());

        while (vad_frame_buffer.size() >= VAD_FRAME_SIZE && g_running) {
            std::vector<float> vad_frame(vad_frame_buffer.begin(), vad_frame_buffer.begin() + VAD_FRAME_SIZE);
            vad_frame_buffer.erase(vad_frame_buffer.begin(), vad_frame_buffer.begin() + VAD_FRAME_SIZE);

            auto vad_result = vad->Detect(vad_frame);
            float vad_prob = vad_result ? vad_result->GetProbability() : 0.0f;

            frame_count++;
            if (frame_count % 10 == 0 && !g_processing) {
                std::cout << "\r" << getTimestamp() << " [VAD] prob=" << std::fixed
                    << std::setprecision(2) << vad_prob
                    << " speaking=" << (is_speaking ? "Y" : "N")
                    << " buffer=" << audio_buffer.size()
                    << " playing=" << (aec_processor.isPlaying() ? "Y" : "N")
                    << "      " << std::flush;
            }

            if (g_processing) {
                if (barge_in_recording && is_speaking) {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());

                    if (vad_prob <= cfg.vad_threshold) {
                        if (post_barge_in_cooldown > 0) {
                            post_barge_in_cooldown--;
                        } else {
                            silence_frames_count++;
                        }
                    } else {
                        silence_frames_count = 0;
                    }
                    continue;
                }

                if (aec_processor.isPlaying() && vad_prob > cfg.vad_threshold) {
                    barge_in_confirm_frames++;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES + BARGE_IN_CONFIRM_THRESHOLD) {
                        pre_buffer.pop_front();
                    }

                    if (barge_in_confirm_frames >= BARGE_IN_CONFIRM_THRESHOLD) {
                        std::cout << "\n" << getTimestamp() << " [Barge-in] 用户打断 (连续"
                            << barge_in_confirm_frames << "帧, prob=" << vad_prob
                            << ")，停止播放\n";
                        aec_processor.clearPlayback();
                        g_barge_in = true;
                        barge_in_recording = true;
                        barge_in_confirm_frames = 0;

                        post_barge_in_cooldown = COOLDOWN_FRAMES;

                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        is_speaking = true;
                        audio_buffer.clear();
                        for (const auto& frame : pre_buffer) {
                            audio_buffer.insert(audio_buffer.end(), frame.begin(), frame.end());
                        }
                        pre_buffer.clear();
                        silence_frames_count = 0;
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
                silence_frames_count = 0;
            } else if (is_speaking) {
                audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());

                if (post_barge_in_cooldown > 0) {
                    post_barge_in_cooldown--;
                } else {
                    silence_frames_count++;
                }

                if (silence_frames_count >= silence_frames_threshold) {
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
                                g_process_thread = std::make_unique<std::thread>([&pipeline_ctx, text]() {
                                    processText(pipeline_ctx, text);
                                });
                            }
                        } else {
                            std::cout << getTimestamp() << " [ASR] 识别完成: (无结果)\n";
                        }
                    }

                    audio_buffer.clear();
                    silence_frames_count = 0;
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

    if (!aec_processor.start()) {
        std::cerr << getTimestamp() << " 错误: 无法启动音频处理\n";
        return 1;
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::lock_guard<std::mutex> lock(g_process_thread_mutex);
        if (g_process_thread && g_process_thread->joinable()) {
            g_process_thread->join();
        }
    }

    aec_processor.stop();

#ifdef USE_MCP
    if (mcp.enabled) {
        if (mcp.registry_poll_thread.joinable()) {
            mcp.registry_poll_thread.join();
        }
        if (mcp.manager) {
            mcp.manager->stopAll();
        }
        std::cout << getTimestamp() << " [MCP] 已清理\n";
    }
#endif

    if (cfg.save_audio && !recorded_audio.empty()) {
        std::cout << getTimestamp() << " [保存音频] " << cfg.audio_file
            << " (" << recorded_audio.size() << " samples, "
            << (recorded_audio.size() / 16000.0f) << " 秒)\n";
        saveWav(cfg.audio_file, recorded_audio, 16000);
    }

    std::cout << "\n" << getTimestamp() << " [已退出]\n";
    return 0;
}
