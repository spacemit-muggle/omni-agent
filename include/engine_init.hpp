/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ENGINE_INIT_HPP
#define ENGINE_INIT_HPP

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <set>

#include "llm_service.h"
#include "vad_service.h"
#include "asr_service.h"
#include "tts_service.h"

#ifdef USE_MCP
#include <mcp_api.hpp>
#include "mcp_helper.hpp"
#endif

struct LLMInitResult {
    std::shared_ptr<spacemit_llm::LLMService> llm;
    std::string system_prompt;
};

LLMInitResult initLLM(const std::string& llm_model, const std::string& llm_url,
    const std::string& default_system_prompt, int max_tokens);

std::shared_ptr<SpacemiT::VadEngine> initVAD(float vad_threshold);

std::shared_ptr<SpacemiT::AsrEngine> initASR();

struct TTSInitResult {
    std::shared_ptr<SpacemiT::TtsEngine> tts;
    int sample_rate;
};

TTSInitResult initTTS(const std::string& tts_type);

#ifdef USE_MCP
struct MCPInitResult {
    bool enabled = false;
    std::unique_ptr<mcp::MCPManager> manager;
    std::string llm_tools_json;
    std::vector<spacemit_llm::ChatMessage> conversation_messages;
    std::thread registry_poll_thread;
    std::mutex tools_mutex;
    std::set<std::string> known_servers;
    MCPConfig config;
};

void initMCP(const std::string& mcp_config_path,
    std::shared_ptr<spacemit_llm::LLMService>& llm,
    std::string& system_prompt,
    MCPInitResult& result);
#endif

#endif  // ENGINE_INIT_HPP
