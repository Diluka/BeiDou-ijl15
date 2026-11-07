#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "../shared/control/Runtime.h"

namespace AgentBridge {
    struct CharacterState {
        std::uint32_t id = 0;
        std::string name;
        std::array<std::uint32_t, 13> values{};
    };

    struct MonsterState {
        std::uint32_t objectId = 0;
        std::uint32_t templateId = 0;
        bool templateValid = false;
        std::string name;
        int x = 0;
        int y = 0;
        bool active = false;
    };

    struct GameState {
        std::string instanceId;
        std::uint64_t sequence = 0;
        std::uint64_t observedAtMs = 0;
        std::uint64_t sceneGeneration = 0;
        std::uint32_t field = 0;
        std::uint32_t stats = 0;
        std::uint32_t user = 0;
        std::uint32_t map = 0;
        CharacterState character;
        int x = 0;
        int y = 0;
        bool mapValid = false;
        bool characterValid = false;
        bool positionValid = false;
        bool loginStage = false;
        bool facingValid = false;
        bool facingLeft = false;
        bool monstersValid = false;
        std::vector<MonsterState> monsters;
        bool inventoryValid = false;
        std::vector<MapleControl::Item> inventory;
    };

    bool Install();
    void CharacterUpdated(std::uintptr_t player);
    bool SemanticAction(const MapleControl::Action& action, std::string& error);
    MapleControl::ActionResult SemanticResult();
    bool ScriptRawKeys(const std::vector<std::string>& keys, unsigned int durationMs, std::string& error);
    bool ScriptKeys(const std::vector<unsigned int>& keys, unsigned int durationMs, std::string& error);
    void ScriptRelease();
    bool ScriptReleased();
    bool ScriptReleasePending();
    bool InvokeSkill(std::uint32_t skillId, int& result, std::string& error);
    bool ScriptLogs(const std::string& scriptId, const std::vector<std::string>& lines);
}
