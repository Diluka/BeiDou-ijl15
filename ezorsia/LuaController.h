#pragma once

#include "AgentBridge.h"

namespace LuaController {
    using Status = MapleControl::Status;

    bool Start(const std::string& scriptId, const std::string& path, unsigned int limitMs,
        const AgentBridge::GameState& state, std::uint64_t generation, std::string& error);
    void Tick(const AgentBridge::GameState& state, std::uint64_t generation, bool sceneChanged);
    void Cancel(const std::string& reason);
    bool Running();
    const Status* CurrentStatus();
}
