#include "stdafx.h"
#include "LuaController.h"

namespace {
class BeiDouAdapter final : public MapleControl::Adapter {
public:
    const char* Backend() const override { return "beidou"; }
    const MapleControl::State& Observe() const override { return state; }
    bool Supports(MapleControl::ActionKind) const override { return true; }
    bool Begin(const MapleControl::Action& action, std::string& error) override {
        return AgentBridge::SemanticAction(action, error);
    }
    MapleControl::ActionResult Result() const override { return AgentBridge::SemanticResult(); }
    void Release() override { AgentBridge::ScriptRelease(); }
    bool Released() const override { return AgentBridge::ScriptReleased(); }
    bool ReleasePending() const override { return AgentBridge::ScriptReleasePending(); }
    bool Logs(const std::string& scriptId, const std::vector<std::string>& lines) override {
        return AgentBridge::ScriptLogs(scriptId, lines);
    }
    bool SupportsRawKeys() const override { return true; }
    bool RawKeys(const std::vector<std::string>& keys, std::uint32_t duration,
        std::string& error) override {
        return AgentBridge::ScriptRawKeys(keys, duration, error);
    }
    bool SupportsSkill() const override { return true; }
    bool Skill(std::uint32_t id, int& result, std::string& error) override {
        return AgentBridge::InvokeSkill(id, result, error);
    }

    void Update(const AgentBridge::GameState& source, std::uint64_t generation) {
        state = MapleControl::State{};
        state.instanceId = source.instanceId;
        state.sequence = source.sequence;
        state.observedAtMs = source.observedAtMs;
        state.sceneGeneration = generation;
        state.inGame = !source.loginStage && !(source.field == 0 && source.stats == 0 && source.user == 0);
        state.ready = source.mapValid && source.characterValid && source.positionValid;
        state.mapKnown = source.mapValid;
        state.mapId = source.map;
        state.characterKnown = source.characterValid;
        state.character.id = source.character.id;
        state.character.name = source.character.name;
        state.character.values = source.character.values;
        state.character.known.fill(source.characterValid);
        state.positionKnown = source.positionValid;
        state.x = source.x;
        state.y = source.y;
        state.facingKnown = source.facingValid;
        state.facingLeft = source.facingLeft;
        state.monstersKnown = source.monstersValid;
        for (const auto& value : source.monsters) {
            MapleControl::Monster monster;
            monster.objectId = value.objectId;
            monster.templateId = value.templateId;
            monster.templateKnown = value.templateValid;
            monster.name = value.name;
            monster.x = value.x;
            monster.y = value.y;
            monster.active = value.active;
            state.monsters.push_back(monster);
        }
        state.inventoryKnown = source.inventoryValid;
        state.inventory = source.inventory;
    }
private:
    MapleControl::State state;
};

BeiDouAdapter adapter;
MapleControl::Runtime runtime(adapter);
MapleControl::Status fileError;
bool haveFileError = false;
}

bool LuaController::Start(const std::string& scriptId, const std::string& path, unsigned int limitMs,
    const AgentBridge::GameState& state, std::uint64_t generation, std::string& error) {
    haveFileError = false;
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) error = "script_file_unavailable";
    std::string source;
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
        source.resize(validSize ? static_cast<std::size_t>(size.QuadPart) : 0);
        DWORD read = 0;
        const bool complete = validSize && (source.empty() ||
            (ReadFile(file, &source[0], static_cast<DWORD>(source.size()), &read, nullptr) && read == source.size()));
        CloseHandle(file);
        if (!complete) error = "script_file_invalid_or_too_large";
    }
    if (!error.empty()) {
        fileError = MapleControl::Status{};
        fileError.id = scriptId;
        fileError.status = "rejected";
        fileError.reason = error;
        fileError.error = error;
        haveFileError = true;
        return false;
    }
    adapter.Update(state, generation);
    return runtime.Start(scriptId, source, limitMs, error);
}

void LuaController::Tick(const AgentBridge::GameState& state, std::uint64_t generation, bool sceneChanged) {
    adapter.Update(state, generation);
    runtime.Tick(sceneChanged);
}

void LuaController::Cancel(const std::string& reason) { runtime.Cancel(reason); }
bool LuaController::Running() { return runtime.Running(); }
const LuaController::Status* LuaController::CurrentStatus() {
    return haveFileError ? &fileError : runtime.CurrentStatus();
}
