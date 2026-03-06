/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOICE_PIPELINE_HPP
#define VOICE_PIPELINE_HPP

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include "llm_service.h"
#include "tts_service.h"
#include "vad_service.h"

#ifdef USE_MCP
#include <mcp_service.hpp>
#endif

using PlaybackCallback = std::function<void(const std::vector<float>& samples, int sample_rate)>;
using IsPlayingCallback = std::function<bool()>;
using ClearPlaybackCallback = std::function<void()>;

struct VoicePipelineContext {
    std::shared_ptr<spacemit_llm::LLMService> llm;
    std::shared_ptr<SpacemiT::TtsEngine> tts;
    std::shared_ptr<SpacemiT::VadEngine> vad;
    int tts_sample_rate;
    std::string system_prompt;
    PlaybackCallback enqueue_playback;
    IsPlayingCallback is_playing;
    ClearPlaybackCallback clear_playback;

    // State variables (references from main)
    std::vector<float>* audio_buffer;
    std::mutex* buffer_mutex;
    int* silence_frames;
    bool* is_speaking;
    std::atomic<bool>* barge_in_recording;
    std::vector<float>* vad_frame_buffer;
    std::deque<std::vector<float>>* pre_buffer;

#ifdef USE_MCP
    mcp::MCPManager* mcp_manager = nullptr;
    std::string* llm_tools_json = nullptr;
    std::mutex* tools_mutex = nullptr;
    std::vector<spacemit_llm::ChatMessage>* conversation_messages = nullptr;
    bool mcp_enabled = false;
#endif
};

void processText(VoicePipelineContext& ctx, const std::string& text);

#endif  // VOICE_PIPELINE_HPP
