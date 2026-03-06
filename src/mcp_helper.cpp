/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef USE_MCP

#include "mcp_helper.hpp"

#include <curl/curl.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "voice_common.hpp"

using json = nlohmann::json;

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
