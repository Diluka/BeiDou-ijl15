#pragma once

#include "AgentBridge.h"

namespace LuaController {
    struct Status {
        std::string id;
        std::string status;
        std::string reason;
        std::string error;
        std::uint64_t resumes = 0;
        std::uint64_t budgetYields = 0;
        double lastResumeUs = 0;
        double maxResumeUs = 0;
    };

    bool Start(const std::string& scriptId, const std::string& path, unsigned int limitMs,
        const AgentBridge::GameState& state, std::uint64_t generation, std::string& error);
    void Tick(const AgentBridge::GameState& state, std::uint64_t generation, bool sceneChanged);
    void Cancel(const std::string& reason);
    bool Running();
    const Status* CurrentStatus();
}
