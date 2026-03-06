/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "engine_init.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "voice_common.hpp"

#ifdef USE_MCP
#include <nlohmann/json.hpp>

#include "mcp_helper.hpp"
using json = nlohmann::json;
#endif

LLMInitResult initLLM(const std::string& llm_model, const std::string& llm_url,
    const std::string& default_system_prompt, int max_tokens) {
    LLMInitResult result;
    result.system_prompt = default_system_prompt;

    result.llm = std::make_shared<spacemit_llm::LLMService>(
        llm_model, llm_url, "EMPTY", result.system_prompt, max_tokens);

    std::cout << getTimestamp() << " [1/5] LLM 后端: " << llm_url << " OK\n";

    return result;
}

std::shared_ptr<SpacemiT::VadEngine> initVAD(float vad_threshold) {
    std::cout << getTimestamp() << " [2/5] 初始化 VAD..." << std::flush;

    auto vad_config = SpacemiT::VadConfig::Preset("silero")
        .withTriggerThreshold(vad_threshold)
        .withStopThreshold(vad_threshold - 0.15f);

    auto vad = std::make_shared<SpacemiT::VadEngine>(vad_config);
    if (!vad->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: VAD 初始化失败\n";
        return nullptr;
    }
    std::cout << " OK (" << vad->GetEngineName() << ")\n";
    return vad;
}

std::shared_ptr<SpacemiT::AsrEngine> initASR() {
    std::cout << getTimestamp() << " [3/5] 初始化 ASR..." << std::flush;
    auto asr = std::make_shared<SpacemiT::AsrEngine>();
    if (!asr->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: ASR 初始化失败\n";
        return nullptr;
    }
    std::cout << " OK\n";
    return asr;
}

TTSInitResult initTTS(const std::string& tts_type) {
    TTSInitResult result;
    std::cout << getTimestamp() << " [4/5] 初始化 TTS (" << tts_type << ")..." << std::flush;

    auto selection = parseEngine(tts_type);

    SpacemiT::TtsConfig tts_cfg;
    tts_cfg.backend = selection.backend;

    if (selection.backend == SpacemiT::BackendType::KOKORO && !selection.voice.empty()) {
        tts_cfg.voice = selection.voice;
    }

    switch (selection.backend) {
        case SpacemiT::BackendType::MATCHA_ZH:
        case SpacemiT::BackendType::MATCHA_EN:
            result.sample_rate = 22050;
            break;
        case SpacemiT::BackendType::MATCHA_ZH_EN:
            result.sample_rate = 16000;
            break;
        case SpacemiT::BackendType::KOKORO:
            result.sample_rate = 24000;
            break;
        default:
            result.sample_rate = 22050;
    }
    tts_cfg.sample_rate = result.sample_rate;

    result.tts = std::make_shared<SpacemiT::TtsEngine>(tts_cfg);
    if (!result.tts->IsInitialized()) {
        std::cerr << "\n" << getTimestamp() << " 错误: TTS 初始化失败\n";
        result.tts = nullptr;
        return result;
    }
    std::cout << " OK\n";
    return result;
}

#ifdef USE_MCP

void initMCP(const std::string& mcp_config_path,
    std::shared_ptr<spacemit_llm::LLMService>& llm,
    std::string& system_prompt,
    MCPInitResult& result,
    const std::string& cli_llm_url,
    const std::string& cli_llm_model) {
    if (mcp_config_path.empty()) return;

    std::cout << getTimestamp() << " [MCP] 加载配置: " << mcp_config_path << "\n";

    if (!loadMCPConfig(mcp_config_path, result.config)) {
        std::cout << getTimestamp() << " [MCP] 配置加载失败，使用默认 LLM\n\n";
        return;
    }

    result.enabled = true;

    // model: CLI > config
    if (cli_llm_model.empty()) {
        llm->update_model(result.config.model);
    }

    // system_prompt: config always overrides (no CLI option for this)
    llm->update_prompt(result.config.system_prompt);
    system_prompt = result.config.system_prompt;

    // url: CLI > config
    if (cli_llm_url.empty() && !result.config.url.empty()) {
        std::string api_base = result.config.url;
        if (api_base.back() == '/') api_base.pop_back();
        if (api_base.size() < 3 || api_base.substr(api_base.size() - 3) != "/v1") {
            api_base += "/v1";
        }
        llm->update_api_settings(api_base, "EMPTY");
    }

    std::cout << getTimestamp() << " [MCP] LLM后端: "
        << (cli_llm_url.empty() ? result.config.url : cli_llm_url) << "\n";
    std::cout << getTimestamp() << " [MCP] 模型: "
        << (cli_llm_model.empty() ? result.config.model : cli_llm_model) << "\n";

    result.manager = std::make_unique<mcp::MCPManager>();

    for (const auto& srv : result.config.servers) {
        result.known_servers.insert(srv.name);
        if (srv.type == "http") {
            mcp::HttpConfig hc;
            hc.url = srv.url;
            result.manager->addHttpServer(srv.name, hc);
            std::cout << getTimestamp()
                << " [MCP] 添加服务器: " << srv.name
                << " (http: " << srv.url << ")\n";
        } else if (srv.type == "stdio") {
            mcp::StdioConfig sc;
            sc.command = srv.command;
            sc.args = srv.args;
            result.manager->addStdioServer(srv.name, sc);
            std::cout << getTimestamp()
                << " [MCP] 添加服务器: " << srv.name
                << " (stdio: " << srv.command << ")\n";
        } else if (srv.type == "socket") {
            mcp::UnixSocketConfig uc;
            uc.socketPath = srv.socketPath;
            result.manager->addUnixSocketServer(srv.name, uc);
            std::cout << getTimestamp()
                << " [MCP] 添加服务器: " << srv.name
                << " (socket: " << srv.socketPath << ")\n";
        }
    }

    if (!result.config.registry_url.empty()) {
        std::cout << getTimestamp() << " [MCP] 从注册中心获取服务: " << result.config.registry_url << "\n";
        auto registry_services = fetchServicesFromRegistry(result.config.registry_url);
        for (const auto& srv : registry_services) {
            if (result.known_servers.find(srv.name) == result.known_servers.end()) {
                result.known_servers.insert(srv.name);
                mcp::HttpConfig hc;
                hc.url = srv.url;
                result.manager->addHttpServer(srv.name, hc);
                std::cout << getTimestamp()
                    << " [MCP] 添加服务器: " << srv.name
                    << " (http: " << srv.url << ")\n";
            }
        }
    }

    std::cout << getTimestamp() << " [MCP] 启动服务器...\n";
    result.manager->startAll();

    if (result.manager->waitForAnyServer(std::chrono::milliseconds(10000))) {
        auto tools = result.manager->getAllTools();
        result.llm_tools_json = convertMCPToolsToString(tools);
        std::cout << getTimestamp() << " [MCP] 已连接 "
            << result.manager->readyServerCount()
            << " 个服务器, " << tools.size() << " 个工具\n";
    } else {
        std::cout << getTimestamp() << " [MCP] 警告: 无可用服务器，继续等待...\n";
    }

    result.conversation_messages.push_back(spacemit_llm::ChatMessage::System(result.config.system_prompt));

    if (!result.config.registry_url.empty()) {
        std::cout << getTimestamp() << " [MCP] 启动注册中心轮询: " << result.config.registry_url << "\n";
        result.registry_poll_thread = std::thread([&result]() {
            while (g_running) {
                auto services = fetchServicesFromRegistry(result.config.registry_url);

                std::set<std::string> registry_services;
                std::map<std::string, std::string> service_urls;
                for (const auto& srv : services) {
                    registry_services.insert(srv.name);
                    service_urls[srv.name] = srv.url;
                }

                std::vector<std::string> to_remove;
                for (const auto& name : result.known_servers) {
                    auto status = result.manager->getStatus(name);

                    if (registry_services.find(name) == registry_services.end()) {
                        if (status.state == mcp::ServerState::Error ||
                            status.state == mcp::ServerState::Disconnected) {
                            to_remove.push_back(name);
                            std::cout << "\n" << getTimestamp() << " [MCP] 服务已下线: " << name << "\n";
                        }
                    } else if (status.state == mcp::ServerState::Error ||
                        status.state == mcp::ServerState::Disconnected) {
                        std::cout << "\n" << getTimestamp() << " [MCP] 尝试重连: " << name << "\n";
                        result.manager->startServer(name);
                    }
                }

                for (const auto& name : to_remove) {
                    result.manager->removeServer(name);
                    result.known_servers.erase(name);
                }

                bool new_services_added = false;
                for (const auto& srv : services) {
                    if (result.known_servers.find(srv.name) == result.known_servers.end()) {
                        mcp::HttpConfig hc;
                        hc.url = srv.url;
                        result.manager->addHttpServer(srv.name, hc);
                        result.manager->startServer(srv.name);
                        result.known_servers.insert(srv.name);
                        new_services_added = true;
                        std::cout << "\n" << getTimestamp()
                            << " [MCP] 发现新服务: " << srv.name
                            << " (" << srv.url << ")\n";
                    }
                }

                if (!to_remove.empty() || new_services_added) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    {
                        std::lock_guard<std::mutex> lock(result.tools_mutex);
                        auto tools = result.manager->getAllTools();
                        result.llm_tools_json = convertMCPToolsToString(tools);
                        std::cout << getTimestamp() << " [MCP] 工具列表已更新: " << tools.size() << " 个工具\n";
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(result.config.registry_poll_interval));
            }
        });
    }

    std::cout << getTimestamp() << " [MCP] 初始化完成\n\n";
}

#endif  // USE_MCP
