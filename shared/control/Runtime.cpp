// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Runtime.h"
#include "../../third_party/lua/src/lua.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace MapleControl {
namespace {
std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Integer(lua_State* lua, const char* name, std::uint64_t value) {
    lua_pushinteger(lua, static_cast<lua_Integer>(value));
    lua_setfield(lua, -2, name);
}

void Number(lua_State* lua, const char* name, double value) {
    lua_pushnumber(lua, value);
    lua_setfield(lua, -2, name);
}

void Boolean(lua_State* lua, const char* name, bool value) {
    lua_pushboolean(lua, value);
    lua_setfield(lua, -2, name);
}

void String(lua_State* lua, const char* name, const std::string& value) {
    lua_pushlstring(lua, value.data(), value.size());
    lua_setfield(lua, -2, name);
}

void ObservedFact(lua_State* lua, const char* name, Fact value) {
    if (value == Fact::Unknown) lua_pushnil(lua);
    else lua_pushboolean(lua, value == Fact::True);
    lua_setfield(lua, -2, name);
}

const char* ActionName(ActionKind kind) {
    switch (kind) {
    case ActionKind::Move: return "move";
    case ActionKind::Face: return "face";
    case ActionKind::Jump: return "jump";
    case ActionKind::Attack: return "attack";
    case ActionKind::Pickup: return "pickup";
    case ActionKind::UseItem: return "use_item";
    case ActionKind::Stop: return "stop";
    }
    return "unsupported";
}

void PushResult(lua_State* lua, const ActionResult& result) {
    lua_createtable(lua, 0, 5);
    Boolean(lua, "dispatched", result.dispatched);
    Boolean(lua, "control_released", result.controlReleased);
    ObservedFact(lua, "settled", result.settled);
    String(lua, "effect", result.effect);
    if (!result.reason.empty()) String(lua, "reason", result.reason);
}

void PushState(lua_State* lua, const State& state) {
    lua_newtable(lua);
    String(lua, "instance_id", state.instanceId);
    Integer(lua, "seq", state.sequence);
    Integer(lua, "observed_at_ms", state.observedAtMs);
    Integer(lua, "scene_generation", state.sceneGeneration);
    Boolean(lua, "ready", state.ready);
    if (state.mapKnown) Integer(lua, "map_id", state.mapId);
    if (state.characterKnown) {
        lua_newtable(lua);
        Integer(lua, "id", state.character.id);
        String(lua, "name", state.character.name);
        static const char* names[]{
            "level", "job", "strength", "dexterity", "intelligence", "luck", "hp", "max_hp",
            "mp", "max_mp", "ability_points", "skill_points", "experience"
        };
        for (std::size_t index = 0; index < state.character.values.size(); ++index)
            if (state.character.known[index]) Integer(lua, names[index], state.character.values[index]);
        lua_setfield(lua, -2, "character");
    }
    if (state.positionKnown) {
        lua_newtable(lua);
        Number(lua, "x", state.x);
        Number(lua, "y", state.y);
        lua_setfield(lua, -2, "position");
    }
    if (state.facingKnown) {
        lua_pushstring(lua, state.facingLeft ? "Left" : "Right");
        lua_setfield(lua, -2, "facing");
    }
    ObservedFact(lua, "settled", state.settled);
    if (state.monstersKnown) {
        lua_createtable(lua, static_cast<int>(state.monsters.size()), 0);
        lua_Integer index = 1;
        for (const auto& monster : state.monsters) {
            lua_newtable(lua);
            Integer(lua, "object_id", monster.objectId);
            if (monster.templateKnown) Integer(lua, "template_id", monster.templateId);
            if (!monster.name.empty()) String(lua, "name", monster.name);
            Number(lua, "x", monster.x);
            Number(lua, "y", monster.y);
            Boolean(lua, "active", monster.active);
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "monsters");
    }
    if (state.inventoryKnown) {
        lua_createtable(lua, static_cast<int>(state.inventory.size()), 0);
        lua_Integer index = 1;
        for (const auto& item : state.inventory) {
            lua_newtable(lua);
            Integer(lua, "inventory_type", item.inventoryType);
            lua_pushinteger(lua, item.slot);
            lua_setfield(lua, -2, "slot");
            Integer(lua, "item_id", item.itemId);
            if (item.quantityKnown) Integer(lua, "quantity", item.quantity);
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "inventory");
    }
    if (state.dropsKnown) {
        lua_createtable(lua, static_cast<int>(state.drops.size()), 0);
        lua_Integer index = 1;
        for (const auto& drop : state.drops) {
            lua_newtable(lua);
            Integer(lua, "object_id", drop.objectId);
            Number(lua, "x", drop.x);
            Number(lua, "y", drop.y);
            if (drop.itemKnown) Integer(lua, "item_id", drop.itemId);
            if (drop.moneyKnown) Integer(lua, "money", drop.money);
            ObservedFact(lua, "landed", drop.landed);
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "drops");
    }
}

unsigned int Duration(lua_State* lua, int argument, double defaultSeconds, bool allowZero) {
    const double seconds = luaL_optnumber(lua, argument, defaultSeconds);
    if (!std::isfinite(seconds) || seconds > 30 || (allowZero ? seconds < 0 : seconds <= 0))
        luaL_error(lua, "duration must be %s 30 seconds", allowZero ? "between 0 and" : "greater than 0 and at most");
    return static_cast<unsigned int>(std::ceil(seconds * 1000));
}
}

struct Runtime::Impl {
    enum class WaitFor { None, Sleep, Action, RawKeys, Release };
    explicit Impl(Adapter& native) : adapter(native) {}
    ~Impl() { if (main != nullptr) lua_close(main); }

    Adapter& adapter;
    lua_State* main = nullptr;
    lua_State* coroutine = nullptr;
    Status report;
    std::uint32_t actorId = 0;
    std::uint64_t limitAt = 0;
    std::uint64_t wakeAt = 0;
    WaitFor wait = WaitFor::None;
    bool finishing = false;
    std::string finalStatus;
    std::string finalReason;
    std::vector<std::string> logs;
    std::size_t logBytes = 0;

    static Impl& Self(lua_State* lua) {
        return **static_cast<Impl**>(lua_getextraspace(lua));
    }

    static void RequireRunning(lua_State* lua) {
        const auto& runtime = Self(lua);
        if (runtime.report.status != "running" || runtime.finishing)
            luaL_error(lua, "game actions require a running script");
    }

    static int Observe(lua_State* lua) {
        PushState(lua, Self(lua).adapter.Observe());
        return 1;
    }

    static int Capabilities(lua_State* lua) {
        const auto& adapter = Self(lua).adapter;
        lua_newtable(lua);
        String(lua, "backend", adapter.Backend());
        for (ActionKind kind : {ActionKind::Move, ActionKind::Face, ActionKind::Jump,
            ActionKind::Attack, ActionKind::Pickup, ActionKind::UseItem, ActionKind::Stop})
            Boolean(lua, ActionName(kind), adapter.Supports(kind));
        Boolean(lua, "raw_keys", adapter.SupportsRawKeys());
        Boolean(lua, "skill", adapter.SupportsSkill());
        return 1;
    }

    bool Begin(ActionKind kind, const char* direction, std::uint32_t durationMs, std::uint32_t itemId,
        lua_State* lua) {
        Action action;
        action.kind = kind;
        action.direction = direction;
        action.durationMs = durationMs;
        action.itemId = itemId;
        std::string error;
        if (!adapter.Begin(action, error)) {
            lua_pushlstring(lua, error.data(), error.size());
            return false;
        }
        return true;
    }

    static int ReturnAction(lua_State* lua, int, lua_KContext) {
        const auto& runtime = Self(lua);
        PushResult(lua, runtime.adapter.Result());
        return 1;
    }

    static int BeginSemantic(lua_State* lua, ActionKind kind, const char* direction,
        std::uint32_t durationMs, std::uint32_t itemId) {
        RequireRunning(lua);
        auto& runtime = Self(lua);
        if (!runtime.adapter.Supports(kind))
            return luaL_error(lua, "unsupported: %s", ActionName(kind));
        if (!runtime.Begin(kind, direction, durationMs, itemId, lua)) return lua_error(lua);
        runtime.wait = WaitFor::Action;
        return lua_yieldk(lua, 0, 0, ReturnAction);
    }

    static const char* Direction(lua_State* lua, bool vertical) {
        const char* direction = luaL_checkstring(lua, 1);
        if (std::strcmp(direction, "left") == 0 || std::strcmp(direction, "right") == 0 ||
            (vertical && (std::strcmp(direction, "up") == 0 || std::strcmp(direction, "down") == 0)))
            return direction;
        luaL_error(lua, "unsupported direction");
        return "";
    }

    static int Move(lua_State* lua) {
        const char* direction = Direction(lua, true);
        const auto duration = Duration(lua, 2, 0.3, false);
        return BeginSemantic(lua, ActionKind::Move, direction, duration, 0);
    }

    static int Face(lua_State* lua) {
        const char* direction = Direction(lua, false);
        return BeginSemantic(lua, ActionKind::Face, direction, 40, 0);
    }

    static int Jump(lua_State* lua) {
        return BeginSemantic(lua, ActionKind::Jump, "", Duration(lua, 1, 0.06, false), 0);
    }

    static int Attack(lua_State* lua) {
        return BeginSemantic(lua, ActionKind::Attack, "", Duration(lua, 1, 0.06, false), 0);
    }

    static int Pickup(lua_State* lua) {
        return BeginSemantic(lua, ActionKind::Pickup, "", Duration(lua, 1, 0.06, false), 0);
    }

    static int UseItem(lua_State* lua) {
        const auto itemId = luaL_checkinteger(lua, 1);
        if (itemId < 1 || itemId > 2147483647) return luaL_error(lua, "item id must be a positive int32");
        return BeginSemantic(lua, ActionKind::UseItem, "", 60, static_cast<std::uint32_t>(itemId));
    }

    static int Sleep(lua_State* lua) {
        RequireRunning(lua);
        auto& runtime = Self(lua);
        runtime.wakeAt = NowMs() + Duration(lua, 1, 0, true);
        runtime.wait = WaitFor::Sleep;
        return lua_yield(lua, 0);
    }

    static int ReturnRelease(lua_State* lua, int, lua_KContext) {
        auto& runtime = Self(lua);
        ActionResult result;
        result.dispatched = true;
        result.controlReleased = runtime.adapter.Released();
        result.settled = runtime.adapter.Observe().settled;
        result.reason = "control_released";
        PushResult(lua, result);
        return 1;
    }

    static int Stop(lua_State* lua) {
        RequireRunning(lua);
        auto& runtime = Self(lua);
        if (!runtime.adapter.Supports(ActionKind::Stop)) return luaL_error(lua, "unsupported: stop");
        runtime.adapter.Release();
        runtime.wait = WaitFor::Release;
        return lua_yieldk(lua, 0, 0, ReturnRelease);
    }

    bool ApplyRawKeys(lua_State* lua, std::uint32_t durationMs) {
        std::vector<std::string> keys;
        if (lua_type(lua, 1) == LUA_TSTRING) {
            std::size_t size = 0;
            const char* value = lua_tolstring(lua, 1, &size);
            keys.emplace_back(value, size);
        }
        else {
            const auto count = lua_rawlen(lua, 1);
            for (std::size_t index = 0; index < count; ++index) {
                lua_rawgeti(lua, 1, static_cast<lua_Integer>(index + 1));
                std::size_t size = 0;
                const char* value = lua_tolstring(lua, -1, &size);
                keys.emplace_back(value, size);
                lua_pop(lua, 1);
            }
        }
        std::string error;
        if (!adapter.RawKeys(keys, durationMs, error)) {
            lua_pushlstring(lua, error.data(), error.size());
            return false;
        }
        return true;
    }

    static void CheckRawKeys(lua_State* lua) {
        if (!Self(lua).adapter.SupportsRawKeys()) luaL_error(lua, "unsupported: raw_keys");
        if (lua_type(lua, 1) == LUA_TSTRING) return;
        luaL_checktype(lua, 1, LUA_TTABLE);
        const auto count = lua_rawlen(lua, 1);
        if (count > 16) luaL_error(lua, "at most 16 keys are supported");
        for (std::size_t index = 0; index < count; ++index) {
            lua_rawgeti(lua, 1, static_cast<lua_Integer>(index + 1));
            luaL_checktype(lua, -1, LUA_TSTRING);
            lua_pop(lua, 1);
        }
    }

    static int Press(lua_State* lua) {
        RequireRunning(lua);
        CheckRawKeys(lua);
        const auto duration = Duration(lua, 2, 0.06, false);
        auto& runtime = Self(lua);
        if (!runtime.ApplyRawKeys(lua, duration)) return lua_error(lua);
        runtime.wait = WaitFor::RawKeys;
        return lua_yieldk(lua, 0, 0, ReturnRelease);
    }

    static int Keys(lua_State* lua) {
        RequireRunning(lua);
        CheckRawKeys(lua);
        const auto duration = Duration(lua, 2, 0.1, false);
        if (!Self(lua).ApplyRawKeys(lua, duration)) return lua_error(lua);
        return 0;
    }

    bool CallSkill(lua_State* lua, std::uint32_t skillId) {
        int nativeResult = 0;
        std::string error;
        if (!adapter.Skill(skillId, nativeResult, error)) {
            lua_pushlstring(lua, error.data(), error.size());
            return false;
        }
        lua_newtable(lua);
        lua_pushinteger(lua, nativeResult);
        lua_setfield(lua, -2, "native_result");
        Boolean(lua, "dispatched", true);
        return true;
    }

    static int Skill(lua_State* lua) {
        RequireRunning(lua);
        if (!Self(lua).adapter.SupportsSkill()) return luaL_error(lua, "unsupported: skill");
        const auto skillId = luaL_checkinteger(lua, 1);
        if (skillId < 1 || skillId > 2147483647) return luaL_error(lua, "skill id must be a positive int32");
        if (!Self(lua).CallSkill(lua, static_cast<std::uint32_t>(skillId))) return lua_error(lua);
        return 1;
    }

    static int Time(lua_State* lua) {
        lua_pushnumber(lua, static_cast<lua_Number>(NowMs()) / 1000);
        return 1;
    }

    static int Log(lua_State* lua) {
        RequireRunning(lua);
        auto& runtime = Self(lua);
        std::size_t size = 0;
        const char* text = luaL_checklstring(lua, 1, &size);
        if (size > 4096 || runtime.logBytes + size > 65536)
            return luaL_error(lua, "script log limit exceeded for this resume");
        runtime.logs.emplace_back(text, size);
        runtime.logBytes += size;
        return 0;
    }

    static void Budget(lua_State* lua, lua_Debug*) {
        if (!lua_isyieldable(lua)) {
            luaL_error(lua, "instruction budget exhausted in a non-yieldable call");
            return;
        }
        ++Self(lua).report.budgetYields;
        lua_yield(lua, 0);
    }

    void Finish(const std::string& status, const std::string& reason) {
        finishing = true;
        finalStatus = status;
        finalReason = reason;
        adapter.Release();
    }

    void CloseLua() {
        if (main != nullptr) lua_close(main);
        main = nullptr;
        coroutine = nullptr;
    }
};

Runtime::Runtime(Adapter& adapter) : impl(new Impl(adapter)) {}
Runtime::~Runtime() = default;

bool Runtime::Start(const std::string& scriptId, const std::string& source,
    std::uint32_t limitMs, std::string& error) {
    if (Running()) {
        error = "script_running";
        return false;
    }
    auto& runtime = *impl;
    runtime.CloseLua();
    runtime.report = Status{};
    runtime.report.id = scriptId;
    runtime.report.status = "rejected";
    runtime.wait = Impl::WaitFor::None;
    runtime.finishing = false;
    runtime.finalStatus.clear();
    runtime.finalReason.clear();
    runtime.logs.clear();
    runtime.logBytes = 0;
    const auto& state = runtime.adapter.Observe();
    runtime.actorId = state.character.id;
    runtime.limitAt = limitMs == 0 ? 0 : NowMs() + limitMs;
    runtime.main = luaL_newstate();
    if (runtime.main == nullptr) {
        runtime.report.reason = error = "lua_state_unavailable";
        return false;
    }
    *static_cast<Impl**>(lua_getextraspace(runtime.main)) = &runtime;
    static const luaL_Reg libraries[]{
        {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {nullptr, nullptr}
    };
    for (const auto* library = libraries; library->func != nullptr; ++library) {
        luaL_requiref(runtime.main, library->name, library->func, 1);
        lua_pop(runtime.main, 1);
    }
    for (const char* name : {"dofile", "loadfile", "load", "print", "pcall", "xpcall",
        "setmetatable", "getmetatable"}) {
        lua_pushnil(runtime.main);
        lua_setglobal(runtime.main, name);
    }
    static const luaL_Reg functions[]{
        {"state", Impl::Observe}, {"capabilities", Impl::Capabilities},
        {"time", Impl::Time}, {"sleep", Impl::Sleep}, {"log", Impl::Log},
        {"move", Impl::Move}, {"face", Impl::Face}, {"jump", Impl::Jump},
        {"attack", Impl::Attack}, {"pickup", Impl::Pickup}, {"use_item", Impl::UseItem},
        {"stop", Impl::Stop}, {"press", Impl::Press}, {"keys", Impl::Keys}, {"skill", Impl::Skill},
        {nullptr, nullptr}
    };
    luaL_newlib(runtime.main, functions);
    lua_setglobal(runtime.main, "game");
    runtime.coroutine = lua_newthread(runtime.main);
    const int loaded = luaL_loadbufferx(runtime.coroutine, source.data(), source.size(), scriptId.c_str(), "t");
    if (loaded != LUA_OK) {
        const char* message = lua_tostring(runtime.coroutine, -1);
        runtime.report.reason = "script_load_error";
        runtime.report.error = message != nullptr ? message : "Lua load failed";
        error = runtime.report.error;
        runtime.CloseLua();
        return false;
    }
    lua_sethook(runtime.coroutine, Impl::Budget, LUA_MASKCOUNT, 50000);
    runtime.report.status = "running";
    return true;
}

void Runtime::Tick(bool sceneChanged) {
    auto& runtime = *impl;
    if (runtime.report.status != "running") return;
    const auto& state = runtime.adapter.Observe();
    if (runtime.finishing) {
        if (!runtime.adapter.Released()) return;
        runtime.report.status = runtime.finalStatus;
        runtime.report.reason = runtime.finalReason;
        runtime.CloseLua();
        return;
    }
    if (state.characterKnown && state.character.id != runtime.actorId) {
        runtime.Finish("cancelled", "character_changed");
        return;
    }
    if (!state.inGame) {
        runtime.Finish("cancelled", "left_game");
        return;
    }
    const auto now = NowMs();
    if (runtime.limitAt != 0 && now >= runtime.limitAt) {
        runtime.Finish("cancelled", "time_limit");
        return;
    }
    if (sceneChanged || runtime.adapter.ReleasePending()) return;
    if (runtime.wait == Impl::WaitFor::Sleep && now < runtime.wakeAt) return;
    if ((runtime.wait == Impl::WaitFor::Action || runtime.wait == Impl::WaitFor::RawKeys ||
        runtime.wait == Impl::WaitFor::Release) && !runtime.adapter.Released()) return;
    runtime.wait = Impl::WaitFor::None;
    const auto started = std::chrono::steady_clock::now();
    int results = 0;
    const int status = lua_resume(runtime.coroutine, nullptr, 0, &results);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ++runtime.report.resumes;
    runtime.report.lastResumeUs = std::chrono::duration<double, std::micro>(elapsed).count();
    runtime.report.maxResumeUs = (std::max)(runtime.report.maxResumeUs, runtime.report.lastResumeUs);
    if (!runtime.logs.empty() && !runtime.adapter.Logs(runtime.report.id, runtime.logs))
        runtime.Finish("cancelled", "script_log_write_failed");
    runtime.logs.clear();
    runtime.logBytes = 0;
    if (runtime.finishing) return;
    if (status == LUA_OK) runtime.Finish("completed", "script_returned");
    else if (status != LUA_YIELD) {
        const char* message = lua_tostring(runtime.coroutine, -1);
        runtime.report.error = message != nullptr ? message : "Lua runtime error";
        runtime.Finish("cancelled", "script_error");
    }
}

void Runtime::Cancel(const std::string& reason) {
    if (Running() && !impl->finishing) impl->Finish("cancelled", reason);
}

bool Runtime::Running() const { return impl->report.status == "running"; }
const Status* Runtime::CurrentStatus() const { return impl->report.id.empty() ? nullptr : &impl->report; }

}
