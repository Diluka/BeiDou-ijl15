#include "stdafx.h"
#include "LuaController.h"
#include "../third_party/lua/src/lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
enum class WaitFor { None, Sleep, Keys };
struct Runtime {
    lua_State* main = nullptr;
    lua_State* coroutine = nullptr;
    LuaController::Status report;
    const AgentBridge::GameState* state = nullptr;
    std::uint64_t generation = 0;
    std::uint32_t actorId = 0;
    ULONGLONG startedAt = 0;
    ULONGLONG limitAt = 0;
    ULONGLONG wakeAt = 0;
    WaitFor wait = WaitFor::None;
    bool finishing = false;
    std::string finalStatus;
    std::string finalReason;
    std::vector<std::string> logs;
    std::size_t logBytes = 0;
    LARGE_INTEGER frequency{};
} runtime;

void RequireRunning(lua_State* lua) {
    if (runtime.report.status != "running" || runtime.finishing)
        luaL_error(lua, "game actions require a running script");
}

void SetInteger(lua_State* lua, const char* name, lua_Integer value) {
    lua_pushinteger(lua, value);
    lua_setfield(lua, -2, name);
}

void SetBoolean(lua_State* lua, const char* name, bool value) {
    lua_pushboolean(lua, value);
    lua_setfield(lua, -2, name);
}

void SetString(lua_State* lua, const char* name, const std::string& value) {
    lua_pushlstring(lua, value.data(), value.size());
    lua_setfield(lua, -2, name);
}

int State(lua_State* lua) {
    const auto& state = *runtime.state;
    lua_newtable(lua);
    SetBoolean(lua, "ready", state.mapValid && state.characterValid && state.positionValid);
    SetInteger(lua, "scene_generation", static_cast<lua_Integer>(runtime.generation));
    if (state.mapValid) SetInteger(lua, "map_id", state.map);
    if (state.characterValid) {
        lua_newtable(lua);
        SetInteger(lua, "id", state.character.id);
        SetString(lua, "name", state.character.name);
        const std::array<const char*, 13> names{
            "level", "job", "strength", "dexterity", "intelligence", "luck", "hp", "max_hp",
            "mp", "max_mp", "ability_points", "skill_points", "experience"
        };
        for (unsigned int index = 0; index < names.size(); ++index)
            SetInteger(lua, names[index], state.character.values[index]);
        lua_setfield(lua, -2, "character");
    }
    if (state.positionValid) {
        lua_newtable(lua);
        SetInteger(lua, "x", state.x);
        SetInteger(lua, "y", state.y);
        lua_setfield(lua, -2, "position");
    }
    if (state.facingValid) {
        lua_pushstring(lua, state.facingLeft ? "Left" : "Right");
        lua_setfield(lua, -2, "facing");
    }
    if (state.monstersValid) {
        lua_createtable(lua, static_cast<int>(state.monsters.size()), 0);
        lua_Integer index = 1;
        for (const auto& monster : state.monsters) {
            lua_newtable(lua);
            SetInteger(lua, "object_id", monster.objectId);
            if (monster.templateValid) SetInteger(lua, "template_id", monster.templateId);
            if (!monster.name.empty()) SetString(lua, "name", monster.name);
            SetInteger(lua, "x", monster.x);
            SetInteger(lua, "y", monster.y);
            SetBoolean(lua, "active", monster.active);
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "monsters");
    }
    return 1;
}

unsigned int KeyCode(lua_State* lua, int argument) {
    std::size_t length = 0;
    const char* key = luaL_checklstring(lua, argument, &length);
    struct NamedKey { const char* name; unsigned int code; };
    static const NamedKey named[]{
        {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"Control_L", VK_LCONTROL}, {"Control_R", VK_RCONTROL}, {"Ctrl", VK_LCONTROL},
        {"Control", VK_LCONTROL}, {"Alt_L", VK_LMENU}, {"Alt_R", VK_RMENU}, {"Alt", VK_LMENU},
        {"Shift_L", VK_LSHIFT}, {"Shift_R", VK_RSHIFT}, {"Shift", VK_LSHIFT},
        {"space", VK_SPACE}, {"Return", VK_RETURN}, {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
        {"Home", VK_HOME}, {"End", VK_END}, {"Prior", VK_PRIOR}, {"Next", VK_NEXT},
        {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"BackSpace", VK_BACK}
    };
    for (const auto& entry : named)
        if (length == std::strlen(entry.name) && std::memcmp(key, entry.name, length) == 0) return entry.code;
    if (length == 1 && key[0] >= 'a' && key[0] <= 'z') return key[0] - 'a' + 'A';
    if (length == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9')))
        return key[0];
    if (length >= 2 && length <= 3 && key[0] == 'F' && key[1] >= '1' && key[1] <= '9') {
        unsigned int number = key[1] - '0';
        if (length == 3) {
            if (key[2] < '0' || key[2] > '9') return luaL_error(lua, "unsupported key name");
            number = number * 10 + key[2] - '0';
        }
        if (number >= 1 && number <= 12) return VK_F1 + number - 1;
    }
    luaL_error(lua, "unsupported key name");
    return 0;
}

unsigned int Duration(lua_State* lua, int argument, double defaultSeconds, bool allowZero) {
    const double seconds = luaL_optnumber(lua, argument, defaultSeconds);
    if (!std::isfinite(seconds) || seconds > 30 || (allowZero ? seconds < 0 : seconds <= 0))
        luaL_error(lua, "duration must be %s 30 seconds", allowZero ? "between 0 and" : "greater than 0 and at most");
    return static_cast<unsigned int>(std::ceil(seconds * 1000));
}

bool ApplyKeys(lua_State* lua, unsigned int durationMs) {
    // Parse Lua values before constructing C++ owners: Lua argument errors use longjmp.
    std::array<unsigned int, 16> parsed{};
    std::size_t count = 0;
    if (lua_type(lua, 1) == LUA_TSTRING) parsed[count++] = KeyCode(lua, 1);
    else {
        luaL_checktype(lua, 1, LUA_TTABLE);
        count = lua_rawlen(lua, 1);
        if (count > parsed.size()) luaL_error(lua, "at most 16 keys are supported");
        for (std::size_t index = 0; index < count; ++index) {
            lua_rawgeti(lua, 1, static_cast<lua_Integer>(index + 1));
            parsed[index] = KeyCode(lua, -1);
            lua_pop(lua, 1);
        }
    }
    const std::vector<unsigned int> keys(parsed.begin(), parsed.begin() + count);
    std::string error;
    if (!AgentBridge::ScriptKeys(keys, durationMs, error)) {
        lua_pushlstring(lua, error.data(), error.size());
        return false;
    }
    return true;
}

int Sleep(lua_State* lua) {
    RequireRunning(lua);
    const unsigned int duration = Duration(lua, 1, 0, true);
    runtime.wakeAt = GetTickCount64() + duration;
    runtime.wait = WaitFor::Sleep;
    return lua_yield(lua, 0);
}

int Press(lua_State* lua) {
    RequireRunning(lua);
    const unsigned int duration = Duration(lua, 2, 0.06, false);
    if (!ApplyKeys(lua, duration)) return lua_error(lua);
    runtime.wait = WaitFor::Keys;
    return lua_yield(lua, 0);
}

int Keys(lua_State* lua) {
    RequireRunning(lua);
    const unsigned int duration = Duration(lua, 2, 0.1, false);
    if (!ApplyKeys(lua, duration)) return lua_error(lua);
    return 0;
}

int Stop(lua_State* lua) {
    RequireRunning(lua);
    AgentBridge::ScriptRelease();
    runtime.wait = WaitFor::Keys;
    return lua_yield(lua, 0);
}

int Time(lua_State* lua) {
    lua_pushnumber(lua, static_cast<lua_Number>(GetTickCount64()) / 1000);
    return 1;
}

bool CallSkill(lua_State* lua, std::uint32_t skillId) {
    int nativeResult = 0;
    std::string error;
    if (!AgentBridge::InvokeSkill(skillId, nativeResult, error)) {
        lua_pushlstring(lua, error.data(), error.size());
        return false;
    }
    lua_newtable(lua);
    SetInteger(lua, "native_result", nativeResult);
    SetBoolean(lua, "dispatched", true);
    return true;
}

int Skill(lua_State* lua) {
    RequireRunning(lua);
    const lua_Integer skill = luaL_checkinteger(lua, 1);
    if (skill < 1 || skill > 2147483647) return luaL_error(lua, "skill id must be a positive int32");
    if (!CallSkill(lua, static_cast<std::uint32_t>(skill))) return lua_error(lua);
    return 1;
}

int Log(lua_State* lua) {
    RequireRunning(lua);
    std::size_t length = 0;
    const char* text = luaL_checklstring(lua, 1, &length);
    if (length > 4096 || runtime.logBytes + length > 65536)
        return luaL_error(lua, "script log limit exceeded for this resume");
    runtime.logs.emplace_back(text, length);
    runtime.logBytes += length;
    return 0;
}

void InstructionBudget(lua_State* lua, lua_Debug*) {
    if (!lua_isyieldable(lua)) {
        luaL_error(lua, "instruction budget exhausted in a non-yieldable call");
        return;
    }
    ++runtime.report.budgetYields;
    lua_yield(lua, 0);
}

void Finish(const std::string& status, const std::string& reason) {
    runtime.finishing = true;
    runtime.finalStatus = status;
    runtime.finalReason = reason;
    AgentBridge::ScriptRelease();
}
}

bool LuaController::Start(const std::string& scriptId, const std::string& path, unsigned int limitMs,
    const AgentBridge::GameState& state, std::uint64_t generation, std::string& error) {
    if (runtime.main != nullptr) lua_close(runtime.main);
    runtime = Runtime{};
    runtime.report.id = scriptId;
    runtime.report.status = "rejected";
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "script_file_unavailable";
        runtime.report.reason = error;
        runtime.report.error = error;
        return false;
    }
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
    std::string source(validSize ? static_cast<std::size_t>(size.QuadPart) : 0, '\0');
    DWORD read = 0;
    const bool complete = validSize && (source.empty() ||
        (ReadFile(file, &source[0], static_cast<DWORD>(source.size()), &read, nullptr) && read == source.size()));
    CloseHandle(file);
    if (!complete) {
        error = "script_file_invalid_or_too_large";
        runtime.report.reason = error;
        runtime.report.error = error;
        return false;
    }
    runtime.actorId = state.character.id;
    runtime.state = &state;
    runtime.generation = generation;
    runtime.startedAt = GetTickCount64();
    runtime.limitAt = limitMs == 0 ? 0 : runtime.startedAt + limitMs;
    QueryPerformanceFrequency(&runtime.frequency);
    runtime.main = luaL_newstate();
    if (runtime.main == nullptr) {
        runtime.report.reason = "lua_state_unavailable";
        error = runtime.report.reason;
        return false;
    }
    const luaL_Reg libraries[]{
        {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {nullptr, nullptr}
    };
    for (const auto* library = libraries; library->func != nullptr; ++library) {
        luaL_requiref(runtime.main, library->name, library->func, 1);
        lua_pop(runtime.main, 1);
    }
    // This controller supports ordinary Lua control flow, not dynamic loaders,
    // protected error recovery, or user finalizers that can outlive script control.
    for (const char* name : {"dofile", "loadfile", "load", "print", "pcall", "xpcall",
        "setmetatable", "getmetatable"}) {
        lua_pushnil(runtime.main);
        lua_setglobal(runtime.main, name);
    }
    const luaL_Reg functions[]{
        {"state", State}, {"sleep", Sleep}, {"press", Press}, {"keys", Keys},
        {"stop", Stop}, {"time", Time}, {"log", Log}, {"skill", Skill}, {nullptr, nullptr}
    };
    luaL_newlib(runtime.main, functions);
    lua_setglobal(runtime.main, "game");
    runtime.coroutine = lua_newthread(runtime.main);
    // The main stack owns the coroutine for the entire script lifetime.
    const int loaded = luaL_loadbufferx(runtime.coroutine, source.data(), source.size(), scriptId.c_str(), "t");
    if (loaded != LUA_OK) {
        const char* message = lua_tostring(runtime.coroutine, -1);
        runtime.report.reason = "script_load_error";
        runtime.report.error = message != nullptr ? message : "Lua load failed";
        error = runtime.report.error;
        lua_close(runtime.main);
        runtime.main = nullptr;
        runtime.coroutine = nullptr;
        return false;
    }
    lua_sethook(runtime.coroutine, InstructionBudget, LUA_MASKCOUNT, 50000);
    runtime.report.status = "running";
    return true;
}

void LuaController::Tick(const AgentBridge::GameState& state, std::uint64_t generation, bool sceneChanged) {
    if (runtime.report.status != "running") return;
    runtime.state = &state;
    runtime.generation = generation;
    const ULONGLONG now = GetTickCount64();
    if (runtime.finishing) {
        if (!AgentBridge::ScriptReleased()) return;
        runtime.report.status = runtime.finalStatus;
        runtime.report.reason = runtime.finalReason;
        lua_close(runtime.main);
        runtime.main = nullptr;
        runtime.coroutine = nullptr;
        return;
    }
    if (state.characterValid && state.character.id != runtime.actorId) {
        Finish("cancelled", "character_changed");
        return;
    }
    if (state.loginStage || (state.field == 0 && state.stats == 0 && state.user == 0)) {
        Finish("cancelled", "left_game");
        return;
    }
    if (runtime.limitAt != 0 && now >= runtime.limitAt) {
        Finish("cancelled", "time_limit");
        return;
    }
    if (sceneChanged) return;
    if (AgentBridge::ScriptReleasePending()) return;
    if (runtime.wait == WaitFor::Sleep && now < runtime.wakeAt) return;
    if (runtime.wait == WaitFor::Keys && !AgentBridge::ScriptReleased()) return;
    runtime.wait = WaitFor::None;
    LARGE_INTEGER started{}, finished{};
    QueryPerformanceCounter(&started);
    int results = 0;
    const int status = lua_resume(runtime.coroutine, nullptr, 0, &results);
    QueryPerformanceCounter(&finished);
    ++runtime.report.resumes;
    runtime.report.lastResumeUs = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000000 /
        static_cast<double>(runtime.frequency.QuadPart);
    runtime.report.maxResumeUs = (std::max)(runtime.report.maxResumeUs, runtime.report.lastResumeUs);
    if (!runtime.logs.empty()) AgentBridge::ScriptLogs(runtime.report.id, runtime.logs);
    runtime.logs.clear();
    runtime.logBytes = 0;
    if (runtime.finishing) return;
    if (status == LUA_OK) Finish("completed", "script_returned");
    else if (status != LUA_YIELD) {
        const char* error = lua_tostring(runtime.coroutine, -1);
        runtime.report.error = error != nullptr ? error : "Lua runtime error";
        Finish("cancelled", "script_error");
    }
}

void LuaController::Cancel(const std::string& reason) {
    if (runtime.report.status == "running") Finish("cancelled", reason);
}

bool LuaController::Running() {
    return runtime.report.status == "running";
}

const LuaController::Status* LuaController::CurrentStatus() {
    return runtime.report.id.empty() ? nullptr : &runtime.report;
}
