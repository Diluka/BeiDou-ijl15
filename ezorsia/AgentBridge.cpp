#include "stdafx.h"
#include "AgentBridge.h"
#include "LuaController.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// These addresses and layouts belong to BeiDou Client V17, EXE SHA256
// 1198fa57ca5a7c489bae43ec13c69681d9cabe0f96762f3dc0357facf2e7d4df.
// The bridge uses the existing client input route; it never builds game packets.
namespace {
struct ClientApplication;
struct InputSystem;
using ApplicationUpdate = void(__thiscall*)(ClientApplication*, int);
using KeyPressed = unsigned int(__thiscall*)(InputSystem*, int);
using InputModifiers = unsigned int(__thiscall*)(InputSystem*);
using InputMessage = void(__stdcall*)(unsigned int, unsigned int, unsigned int);
struct ClientPlayer;
using ActiveSkill = int(__thiscall*)(ClientPlayer*, int, int, int*);

auto originalUpdate = reinterpret_cast<ApplicationUpdate>(0x009F84D0);
auto originalKeyPressed = reinterpret_cast<KeyPressed>(0x0059A25A);
const auto dispatchInput = reinterpret_cast<InputMessage>(0x009F97B7);
const auto inputModifiers = reinterpret_cast<InputModifiers>(0x0059B1EB);
const auto activeSkill = reinterpret_cast<ActiveSkill>(0x00966F7A);

using AgentBridge::GameState;

std::string directory;
std::string instanceId;
std::string runId;
std::string commandId;
std::string commandStatus = "idle";
std::string commandReason;
std::string rejectedId;
std::string rejectedReason;
std::string lastConsumedId;
std::string pendingStatus;
std::string pendingReason;
std::array<bool, 256> held{};
std::array<bool, 256> owned{};
std::array<unsigned int, 256> scanCodes{};
std::array<std::uint32_t, 256> nativeScanCodes{};
std::uint32_t nativeScanCodesOwner = 0;
enum class InputOwner { None, External, Script };
InputOwner inputOwner = InputOwner::None;
bool active = false;
bool scriptCommand = false;
bool haveSkillResult = false;
std::uint32_t lastSkillId = 0;
int lastSkillResult = 0;
const GameState* currentState = nullptr;
std::uint32_t controlledCharacter = 0;
std::uint32_t updatedCharacter = 0;
std::uint32_t releaseCharacter = 0;
std::uint64_t characterUpdates = 0;
std::uint64_t releaseAtCharacterUpdate = 0;
bool releaseNeedsCharacterUpdate = false;
bool releasing = false;
bool haveScene = false;
GameState previousState;
std::uint64_t sceneGeneration = 1;
std::uint64_t sequence = 0;
std::uint64_t inputEvents = 0;
MapleControl::ActionResult semanticResult;
ULONGLONG deadline = 0;
ULONGLONG lastPublishedAt = 0;

template <typename T>
bool ReadAt(std::uintptr_t address, T& value) {
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        &value, sizeof(value), &count) && count == sizeof(value);
}

std::string Quote(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char byte : text) {
        if (byte == '"' || byte == '\\') out << '\\' << byte;
        else if (byte < 0x20) {
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(byte) << std::dec;
        }
        else out << byte;
    }
    out << '"';
    return out.str();
}

std::string Utf8Name(const char* name, int length) {
    const int wideLength = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, name, length, nullptr, 0);
    if (wideLength == 0) return {};
    std::wstring wide(wideLength, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, name, length, &wide[0], wideLength);
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
        nullptr, 0, nullptr, nullptr);
    std::string result(utf8Length, '\0');
    if (utf8Length != 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
        &result[0], utf8Length, nullptr, nullptr);
    return result;
}

bool SecureValue(std::uintptr_t address, std::uint32_t& map) {
    std::array<unsigned char, 12> guards{};
    if (!ReadAt(address, guards)) return false;
    std::uint32_t pointer = 0;
    std::memcpy(&pointer, guards.data() + 8, sizeof(pointer));
    std::array<unsigned char, 10> encrypted{};
    if (!ReadAt(pointer, encrypted)) return false;
    unsigned char key = encrypted[4];
    std::uint16_t checksum = 0x9A65;
    map = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        if (key == 0) key = 42;
        map |= static_cast<std::uint32_t>(encrypted[index] ^ key) << (index * 8);
        key = static_cast<unsigned char>(key + encrypted[index] + 42);
        checksum = static_cast<std::uint16_t>((checksum << 3) | ((checksum >> 13) + key));
    }
    std::uint16_t expected = 0;
    std::memcpy(&expected, encrypted.data() + 8, sizeof(expected));
    return guards[0] == encrypted[5] && guards[4] == encrypted[6] && checksum == expected;
}

bool SecureInteger(std::uintptr_t address, std::uint32_t& value) {
    std::array<std::uint32_t, 3> data{};
    if (!ReadAt(address, data)) return false;
    const std::uint32_t mixed = data[0] ^ 0xBAADF00D;
    const std::uint32_t checksum = data[1] + ((mixed >> 5) | (mixed << 27));
    value = data[0] ^ ((data[1] << 5) | (data[1] >> 27));
    return checksum == data[2];
}

bool CaptureMonsters(std::vector<AgentBridge::MonsterState>& monsters) {
    std::uint32_t pool = 0;
    std::uint32_t vtable = 0;
    std::array<std::uint32_t, 3> membership{};
    if (!ReadAt(0x00BEBFA4, pool) || pool == 0 || !ReadAt(pool, vtable) ||
        vtable != 0x00AF8378 || !ReadAt(pool + 0x24, membership) || membership[0] > 512) return false;
    auto position = membership[1];
    while (position != 0 && monsters.size() < membership[0]) {
        std::uint32_t object = 0;
        std::array<std::uint32_t, 3> vtables{};
        std::array<int, 2> coordinates{};
        std::uint32_t entry = 0;
        AgentBridge::MonsterState monster;
        std::uint32_t activeValue = 0;
        if (!ReadAt(position + 4, object) || object == 0 || !ReadAt(object, vtables) ||
            vtables != std::array<std::uint32_t, 3>{0x00AF8270, 0x00AF824C, 0x00AF8248} ||
            !ReadAt(object + 0x510, coordinates) || !SecureInteger(object + 0x17C, monster.objectId) ||
            !SecureInteger(object + 0x124, activeValue) || !ReadAt(object + 0x188, entry)) return false;
        monster.x = coordinates[0];
        monster.y = coordinates[1];
        monster.active = activeValue != 0;
        if (entry != 0) {
            if (!SecureInteger(entry + 0x0C, monster.templateId)) return false;
            monster.templateValid = true;
            std::uint32_t name = 0;
            std::uint32_t nameLength = 0;
            if (ReadAt(entry + 0x30, name) && name != 0 && ReadAt(name - 4, nameLength) &&
                nameLength > 0 && nameLength <= 512) {
                std::array<char, 513> bytes{};
                SIZE_T read = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(name), bytes.data(),
                    nameLength + 1, &read) && read == nameLength + 1 && bytes[nameLength] == 0)
                    monster.name = Utf8Name(bytes.data(), static_cast<int>(nameLength));
            }
        }
        monsters.push_back(monster);
        std::uint32_t node = 0;
        if (!ReadAt(position - 12, node)) return false;
        position = node == 0 ? 0 : node + 16;
    }
    std::array<std::uint32_t, 3> after{};
    return position == 0 && monsters.size() == membership[0] && ReadAt(pool + 0x24, after) &&
        after == membership;
}

bool CaptureInventory(std::uint32_t stats, std::vector<MapleControl::Item>& inventory) {
    for (int type = 1; type <= 5; ++type) {
        std::uint32_t array = 0;
        std::uint32_t length = 0;
        if (!ReadAt(stats + 0x447 + type * 4, array) || array == 0 ||
            !ReadAt(array - 4, length) || length == 0 || length > 257) return false;
        for (std::uint32_t slot = 1; slot < length; ++slot) {
            std::uint32_t object = 0;
            if (!ReadAt(array + slot * 8 + 4, object)) return false;
            if (object == 0) continue;
            std::uint32_t vtable = 0;
            MapleControl::Item item;
            item.inventoryType = type;
            item.slot = static_cast<int>(slot);
            if (!ReadAt(object, vtable) ||
                (vtable != 0x00AF310C && vtable != 0x00AF318C && vtable != 0x00AF3204) ||
                !SecureValue(object + 0x0C, item.itemId)) return false;
            if (vtable == 0x00AF310C) {
                item.quantity = 1;
                item.quantityKnown = true;
            }
            else if (vtable == 0x00AF318C) {
                std::array<unsigned char, 8> data{};
                if (!ReadAt(object + 0x28, data)) return false;
                std::uint32_t checksum = 0xBAADF00D;
                for (unsigned int index = 0; index < 2; ++index) {
                    const auto mixed = data[index] ^ checksum;
                    checksum = data[index + 2] + ((mixed >> 5) | (mixed << 27));
                }
                std::uint32_t expected = 0;
                std::memcpy(&expected, data.data() + 4, sizeof(expected));
                if (checksum != expected) return false;
                item.quantity = (data[0] ^ data[2]) | ((data[1] ^ data[3]) << 8);
                item.quantityKnown = true;
            }
            inventory.push_back(item);
        }
    }
    return true;
}

std::uint64_t UnixMilliseconds() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const auto ticks = (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    return (ticks - 116444736000000000ULL) / 10000;
}

bool BindingKey(unsigned char type, std::uint32_t id, unsigned int& key, std::string& error) {
    std::uint32_t mapped = 0;
    std::uint32_t input = 0;
    std::uint32_t vtable = 0;
    std::array<unsigned char, 89 * 5> table{};
    if (!ReadAt(0x00BED5A0, mapped) || mapped == 0 || !ReadAt(mapped, vtable) ||
        vtable != 0x00AF5650 || !ReadAt(mapped + 4, table) ||
        !ReadAt(0x00BEC33C, input) || input == 0) {
        error = "native_binding_unavailable";
        return false;
    }
    for (unsigned int index = 0; index < 89; ++index) {
        std::uint32_t actionId = 0;
        std::memcpy(&actionId, table.data() + index * 5 + 1, sizeof(actionId));
        if (table[index * 5] != type || actionId != id) continue;
        // Extended navigation keys share the low seven DIK bits used by CFuncKeyMappedMan.
        unsigned int scan = index;
        if (index == 71 || index == 72 || index == 73 || index == 75 || index == 77 ||
            index == 79 || index == 80 || index == 81 || index == 82 || index == 83) scan |= 0x80;
        if (!ReadAt(input + 0x424 + scan * 4, key) || key == 0 || key > 255) continue;
        return true;
    }
    error = type == 2 ? "unsupported: item_not_bound" : "unsupported: native_action_not_bound";
    return false;
}

GameState CaptureState() {
    GameState state;
    ReadAt(0x00BEDED4, state.field);
    std::uint32_t stageType = 0;
    state.loginStage = state.field != 0 && ReadAt(state.field, stageType) && stageType == 0x00AF6B24;
    if (state.field != 0) state.mapValid = SecureValue(state.field + 0x138, state.map);
    std::uint32_t context = 0;
    if (ReadAt(0x00BE7918, context) && context != 0 &&
        ReadAt(context + 0x20B8, state.stats) && state.stats != 0) {
        std::array<unsigned char, 0x100> data{};
        if (ReadAt(state.stats, data)) {
            std::memcpy(&state.character.id, data.data(), sizeof(state.character.id));
            int nameLength = 0;
            while (nameLength < 13 && data[4 + nameLength] != 0) ++nameLength;
            state.character.name = Utf8Name(reinterpret_cast<const char*>(data.data() + 4), nameLength);
            const std::array<unsigned int, 13> offsets{
                0x33, 0x39, 0x41, 0x49, 0x51, 0x59, 0x61, 0x69,
                0x71, 0x79, 0x81, 0x89, 0x91
            };
            state.characterValid = true;
            for (unsigned int field = 0; field < offsets.size(); ++field) {
                const unsigned int size = field == 0 ? 1 : field == 12 ? 4 : 2;
                const unsigned int offset = offsets[field];
                std::uint32_t checksum = 0xBAADF00D;
                std::uint32_t value = 0;
                if (size == 4) {
                    std::uint32_t key = 0;
                    std::uint32_t encrypted = 0;
                    std::memcpy(&key, data.data() + offset, sizeof(key));
                    std::memcpy(&encrypted, data.data() + offset + 4, sizeof(encrypted));
                    value = key ^ ((encrypted << 5) | (encrypted >> 27));
                    const std::uint32_t mixed = key ^ checksum;
                    checksum = encrypted + ((mixed >> 5) | (mixed << 27));
                }
                else for (unsigned int index = 0; index < size; ++index) {
                    const unsigned char key = data[offset + index];
                    const unsigned char encrypted = data[offset + size + index];
                    const std::uint32_t mixed = key ^ checksum;
                    checksum = encrypted + ((mixed >> 5) | (mixed << 27));
                    value |= static_cast<std::uint32_t>(key ^ encrypted) << (index * 8);
                }
                std::uint32_t expected = 0;
                std::memcpy(&expected, data.data() + offset + size * 2, sizeof(expected));
                if (checksum != expected) state.characterValid = false;
                state.character.values[field] = value;
            }
        }
    }
    if (ReadAt(0x00BEBF98, state.user) && state.user != 0) {
        std::array<std::uint32_t, 2> vtables{};
        std::array<int, 2> position{};
        state.positionValid = ReadAt(state.user, vtables) && vtables[0] == 0x00B3D20C &&
            vtables[1] == 0x00B3D1E8 && ReadAt(state.user + 0x116C, position);
        if (state.positionValid) {
            state.x = position[0];
            state.y = position[1];
            std::uint32_t facing = 0;
            state.facingValid = ReadAt(state.user + 0x570, facing);
            state.facingLeft = (facing & 1) != 0;
        }
    }
    if (state.mapValid) {
        state.monstersValid = CaptureMonsters(state.monsters);
        if (!state.monstersValid) state.monsters.clear();
    }
    if (state.characterValid) {
        state.inventoryValid = CaptureInventory(state.stats, state.inventory);
        if (!state.inventoryValid) state.inventory.clear();
    }
    return state;
}

unsigned int __fastcall KeyPressedHook(InputSystem* input, void*, int key) {
    if (key >= 0 && key < 256 && owned[key]) return held[key] ? 1 : 0;
    // The native input system exposes both side-specific and aggregate modifier keys.
    if (key == VK_SHIFT && (owned[VK_LSHIFT] || owned[VK_RSHIFT]))
        return held[VK_LSHIFT] || held[VK_RSHIFT] || originalKeyPressed(input, key);
    if (key == VK_CONTROL && (owned[VK_LCONTROL] || owned[VK_RCONTROL]))
        return held[VK_LCONTROL] || held[VK_RCONTROL] || originalKeyPressed(input, key);
    if (key == VK_MENU && (owned[VK_LMENU] || owned[VK_RMENU]))
        return held[VK_LMENU] || held[VK_RMENU] || originalKeyPressed(input, key);
    return originalKeyPressed(input, key);
}

bool SendKey(unsigned int key, bool down) {
    std::uint32_t windowManager = 0;
    std::uint32_t input = 0;
    if (!ReadAt(0x00BEC20C, windowManager) || windowManager == 0 ||
        !ReadAt(0x00BEC33C, input) || input == 0) return false;
    unsigned int flags = ((scanCodes[key] & 0x7F) | ((scanCodes[key] & 0x80) << 1)) << 16;
    // 0x59B1EB's low flags encode modifiers, not a Win32 repeat count.
    flags |= inputModifiers(reinterpret_cast<InputSystem*>(input));
    if (!down) flags |= 0x80000000;
    // Native keyboard polling emits the aggregate VK and retains the side in DIK/flags.
    const unsigned int messageKey = key == VK_LSHIFT || key == VK_RSHIFT ? VK_SHIFT :
        key == VK_LCONTROL || key == VK_RCONTROL ? VK_CONTROL :
        key == VK_LMENU || key == VK_RMENU ? VK_MENU : key;
    dispatchInput(0x100, messageKey, flags);
    ++inputEvents;
    return true;
}

void BeginRelease(const std::string& status, const std::string& reason) {
    releaseNeedsCharacterUpdate = false;
    for (bool key : owned) releaseNeedsCharacterUpdate = releaseNeedsCharacterUpdate || key;
    releaseCharacter = controlledCharacter;
    releaseAtCharacterUpdate = characterUpdates;
    for (unsigned int key = 0; key < held.size(); ++key) if (held[key]) {
        held[key] = false;
        SendKey(key, false);
    }
    active = false;
    releasing = true;
    pendingStatus = status;
    pendingReason = reason;
}

bool ApplyDesiredKeys(const std::vector<unsigned int>& keys, unsigned int durationMs, std::string& error) {
    if (keys.empty()) {
        BeginRelease({}, {});
        return true;
    }
    if (!currentState->mapValid || !currentState->characterValid || !currentState->positionValid ||
        currentState->character.values[6] == 0) {
        error = "state_unavailable_or_dead";
        return false;
    }
    std::uint32_t input = 0;
    if (!ReadAt(0x00BEC33C, input) || input == 0) {
        error = "input_route_unavailable";
        return false;
    }
    if (nativeScanCodesOwner != input) {
        if (!ReadAt(input + 0x24, nativeScanCodes)) {
            error = "input_key_map_unavailable";
            return false;
        }
        nativeScanCodesOwner = input;
    }
    std::array<bool, 256> desired{};
    for (unsigned int key : keys) {
        if (key == 0 || key > 255 || nativeScanCodes[key] == 0 || nativeScanCodes[key] > 255) {
            error = "unsupported_native_key";
            return false;
        }
        desired[key] = true;
        scanCodes[key] = nativeScanCodes[key];
    }
    // A nonblocking Lua keys update replaces the desired set without flickering unchanged keys.
    releasing = false;
    pendingStatus.clear();
    pendingReason.clear();
    for (unsigned int key = 0; key < held.size(); ++key) if (held[key] && !desired[key]) {
        held[key] = false;
        SendKey(key, false);
    }
    for (unsigned int key = 0; key < held.size(); ++key) if (!held[key] && desired[key]) {
        owned[key] = true;
        held[key] = true;
        if (!SendKey(key, true)) {
            BeginRelease({}, {});
            error = "input_route_unavailable";
            return false;
        }
    }
    active = true;
    controlledCharacter = currentState->user;
    deadline = GetTickCount64() + durationMs;
    return true;
}

bool Publish(const GameState& state) {
    const bool ready = state.mapValid && state.characterValid && state.positionValid;
    std::ostringstream out;
    out << "{\"protocol\":1,\"instance_id\":" << Quote(instanceId)
        << ",\"pid\":" << GetCurrentProcessId() << ",\"run_id\":" << Quote(runId)
        << ",\"seq\":" << state.sequence << ",\"observed_at_ms\":" << state.observedAtMs
        << ",\"scene_generation\":" << sceneGeneration
        << ",\"ready\":" << (ready ? "true" : "false") << ",\"map_id\":";
    if (state.mapValid) out << state.map;
    else out << "null";
    out << ",\"character\":";
    if (state.characterValid) {
        out << "{\"id\":" << state.character.id << ",\"name\":" << Quote(state.character.name);
        const std::array<const char*, 13> names{
            "level", "job", "strength", "dexterity", "intelligence", "luck", "hp", "max_hp",
            "mp", "max_mp", "ability_points", "skill_points", "experience"
        };
        for (unsigned int index = 0; index < names.size(); ++index)
            out << ',' << Quote(names[index]) << ':' << state.character.values[index];
        out << '}';
    }
    else out << "null";
    out << ",\"position\":";
    if (state.positionValid) out << "{\"x\":" << state.x << ",\"y\":" << state.y << '}';
    else out << "null";
    out << ",\"facing\":";
    if (state.facingValid) out << Quote(state.facingLeft ? "Left" : "Right");
    else out << "null";
    out << ",\"monsters\":";
    if (state.monstersValid) {
        out << '[';
        bool separator = false;
        for (const auto& monster : state.monsters) {
            if (separator) out << ',';
            separator = true;
            out << "{\"object_id\":" << monster.objectId << ",\"template_id\":";
            if (monster.templateValid) out << monster.templateId;
            else out << "null";
            out << ",\"name\":" << (monster.name.empty() ? "null" : Quote(monster.name))
                << ",\"x\":" << monster.x << ",\"y\":" << monster.y
                << ",\"active\":" << (monster.active ? "true" : "false") << '}';
        }
        out << ']';
    }
    else out << "null";
    out << ",\"backend\":\"beidou\",\"capabilities\":{\"move\":true,\"face\":true,\"jump\":true,\"attack\":true,\"pickup\":true,\"use_item\":true,\"stop\":true,\"raw_keys\":true,\"skill\":true,\"use_item_requires_binding\":true},\"inventory\":";
    if (state.inventoryValid) {
        out << '[';
        bool separator = false;
        for (const auto& item : state.inventory) {
            if (separator) out << ',';
            separator = true;
            out << "{\"inventory_type\":" << item.inventoryType << ",\"slot\":" << item.slot
                << ",\"item_id\":" << item.itemId << ",\"quantity\":";
            if (item.quantityKnown) out << item.quantity;
            else out << "null";
            out << '}';
        }
        out << ']';
    }
    else out << "null";
    out << ",\"held_keys\":[";
    bool separator = false;
    for (unsigned int key = 0; key < held.size(); ++key) if (held[key]) {
        if (separator) out << ',';
        out << key;
        separator = true;
    }
    out << "],\"command\":{\"id\":" << (commandId.empty() ? "null" : Quote(commandId))
        << ",\"status\":" << Quote(commandStatus) << ",\"reason\":"
        << (commandReason.empty() ? "null" : Quote(commandReason)) << "},\"rejected_command\":";
    if (rejectedReason.empty()) out << "null";
    else out << "{\"id\":" << (rejectedId.empty() ? "null" : Quote(rejectedId))
        << ",\"status\":\"rejected\",\"reason\":" << Quote(rejectedReason) << '}';
    out << ",\"script\":";
    const auto* script = LuaController::CurrentStatus();
    if (script == nullptr) out << "null";
    else out << "{\"id\":" << Quote(script->id) << ",\"status\":" << Quote(script->status)
        << ",\"reason\":" << (script->reason.empty() ? "null" : Quote(script->reason))
        << ",\"error\":" << (script->error.empty() ? "null" : Quote(script->error))
        << ",\"resumes\":" << script->resumes << ",\"budget_yields\":" << script->budgetYields
        << ",\"last_resume_us\":" << script->lastResumeUs << ",\"max_resume_us\":" << script->maxResumeUs << '}';
    out << ",\"last_skill\":";
    if (haveSkillResult) out << "{\"skill_id\":" << lastSkillId << ",\"native_result\":" << lastSkillResult << '}';
    else out << "null";
    const auto actionResult = AgentBridge::SemanticResult();
    out << ",\"action_result\":{\"dispatched\":" << (actionResult.dispatched ? "true" : "false")
        << ",\"control_released\":" << (actionResult.controlReleased ? "true" : "false")
        << ",\"settled\":";
    if (actionResult.settled == MapleControl::Fact::Unknown) out << "null";
    else out << (actionResult.settled == MapleControl::Fact::True ? "true" : "false");
    out << ",\"effect\":" << Quote(actionResult.effect)
        << ",\"reason\":" << Quote(actionResult.reason) << '}';
    out << ",\"input_events\":" << inputEvents << "}\n";
    const std::string bytes = out.str();
    const std::string temporary = directory + "\\state.tmp";
    const HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
        nullptr) && written == bytes.size();
    const bool closed = CloseHandle(file) != FALSE;
    return complete && closed && MoveFileExA(temporary.c_str(), (directory + "\\state.json").c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Tick() {
    const ULONGLONG now = GetTickCount64();
    GameState state = CaptureState();
    const bool changed = haveScene && (state.field != previousState.field ||
        state.stats != previousState.stats || state.mapValid != previousState.mapValid ||
        (state.mapValid && state.map != previousState.map) ||
        (state.characterValid && state.character.id != previousState.character.id));
    if (changed) ++sceneGeneration;
    state.instanceId = instanceId;
    state.sequence = ++sequence;
    state.observedAtMs = UnixMilliseconds();
    state.sceneGeneration = sceneGeneration;
    previousState = state;
    currentState = &previousState;
    haveScene = true;

    // CallUpdate may return without advancing its 30ms character step. Confirm the actual consumer.
    const bool characterConsumedRelease = updatedCharacter == releaseCharacter &&
        characterUpdates > releaseAtCharacterUpdate;
    const bool oldCharacterExited = state.loginStage ||
        (state.field == 0 && state.stats == 0 && state.user == 0) ||
        (state.characterValid && state.positionValid && state.user != releaseCharacter);
    if (releasing && (!releaseNeedsCharacterUpdate || characterConsumedRelease || oldCharacterExited)) {
        releasing = false;
        owned.fill(false);
        inputOwner = InputOwner::None;
        controlledCharacter = 0;
        if (!pendingStatus.empty()) {
            commandStatus = pendingStatus;
            commandReason = pendingReason;
        }
        pendingStatus.clear();
        pendingReason.clear();
    }
    const bool ready = state.mapValid && state.characterValid && state.positionValid;
    if (active && (changed || !ready || state.character.values[6] == 0)) {
        if (inputOwner == InputOwner::Script) BeginRelease({}, {});
        else BeginRelease("cancelled", changed ? "scene_changed" : !ready ? "state_unavailable" : "dead");
    }
    else if (active && now >= deadline) {
        if (inputOwner == InputOwner::Script) BeginRelease({}, {});
        else BeginRelease("completed", "duration_elapsed");
    }

    const std::string requestPath = directory + "\\command.txt";
    const HANDLE request = CreateFileA(requestPath.c_str(), GENERIC_READ, FILE_SHARE_READ |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (request != INVALID_HANDLE_VALUE) {
        std::array<char, 1025> buffer{};
        DWORD bytes = 0;
        const bool read = ReadFile(request, buffer.data(), 1024, &bytes, nullptr) != FALSE;
        CloseHandle(request);
        DeleteFileA(requestPath.c_str());
        unsigned int protocol = 0;
        std::uint64_t generation = 0;
        unsigned int value = 0;
        std::string targetInstance;
        std::string id;
        std::string operation;
        std::istringstream command(std::string(buffer.data(), bytes));
        const bool header = read && bytes < 1024 &&
            static_cast<bool>(command >> protocol >> targetInstance >> generation >> id >> operation >> value);
        if (!header) {
            rejectedId = id;
            rejectedReason = "invalid_command";
        }
        else if (id != lastConsumedId) {
            lastConsumedId = id;
            rejectedId.clear();
            rejectedReason.clear();
            std::vector<unsigned int> keys;
            std::string scriptId;
            bool valid = protocol == 1 && targetInstance == instanceId &&
                !id.empty() && id.size() <= 64 && id.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos;
            if (operation == "keys") {
                unsigned int key = 0;
                while (command >> key) keys.push_back(key);
                valid = valid && command.eof() && generation == sceneGeneration &&
                    value >= 1 && value <= 30000 && !keys.empty() && keys.size() <= 16 &&
                    ready && state.character.values[6] != 0;
            }
            else {
                if (operation == "script") command >> scriptId;
                std::string extra;
                const bool hasExtra = static_cast<bool>(command >> extra);
                if (operation == "script") valid = valid && !hasExtra && generation == sceneGeneration &&
                    scriptId.size() == 32 && scriptId.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos &&
                    ready && state.character.values[6] != 0;
                else if (operation == "skill") valid = valid && !hasExtra && generation == sceneGeneration &&
                    value >= 1 && value <= 2147483647 && ready && state.character.values[6] != 0;
                else valid = valid && operation == "stop" && value == 0 && !hasExtra;
            }
            const bool busy = operation != "stop" && (active || releasing || LuaController::Running());
            if (!valid || busy) {
                rejectedId = id;
                rejectedReason = valid ? "busy" : "invalid_or_stale_command";
                if (!active && !releasing && !LuaController::Running()) {
                    commandId = id;
                    commandStatus = "rejected";
                    commandReason = rejectedReason;
                }
            }
            else {
                commandId = id;
                commandStatus = "running";
                commandReason.clear();
                if (operation == "stop") {
                    LuaController::Cancel("stopped");
                    scriptCommand = false;
                    BeginRelease("completed", "stopped");
                }
                else if (operation == "script") {
                    std::string error;
                    scriptCommand = LuaController::Start(scriptId, directory + "\\scripts\\" + scriptId + ".lua",
                        value, state, sceneGeneration, error);
                    if (!scriptCommand) {
                        commandStatus = "rejected";
                        commandReason = "script_load_error";
                    }
                }
                else if (operation == "skill") {
                    int result = 0;
                    std::string error;
                    if (!AgentBridge::InvokeSkill(value, result, error)) BeginRelease("rejected", error);
                    else BeginRelease("completed", "native_returned");
                }
                else {
                    inputOwner = InputOwner::External;
                    std::string error;
                    if (!ApplyDesiredKeys(keys, value, error)) BeginRelease("rejected", error);
                }
            }
        }
    }
    LuaController::Tick(state, sceneGeneration, changed);
    if (scriptCommand && !LuaController::Running()) {
        const auto* script = LuaController::CurrentStatus();
        commandStatus = script->status;
        commandReason = script->reason;
        scriptCommand = false;
    }
    if (now - lastPublishedAt >= 25) {
        if (!Publish(state)) {
            if (LuaController::Running()) LuaController::Cancel("state_publish_failed");
            else if (active) BeginRelease("cancelled", "state_publish_failed");
        }
        lastPublishedAt = now;
    }
}

void __fastcall UpdateHook(ClientApplication* app, void*, int now) {
    originalUpdate(app, now);
    Tick();
}
}

bool AgentBridge::Install() {
    std::array<char, MAX_PATH> path{};
    const DWORD length = GetEnvironmentVariableA("MAPLE_BRIDGE_DIR", path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return true;
    if (length >= path.size()) return false;
    directory.assign(path.data(), length);
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::array<char, 128> run{};
    const DWORD runLength = GetEnvironmentVariableA("RUN_ID", run.data(), static_cast<DWORD>(run.size()));
    if (runLength < run.size()) runId.assign(run.data(), runLength);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    std::ostringstream identity;
    identity << GetCurrentProcessId() << '-' << std::hex << created.dwHighDateTime << '-'
        << created.dwLowDateTime;
    instanceId = identity.str();
    if (!Memory::SetHook(true, reinterpret_cast<void**>(&originalKeyPressed), KeyPressedHook)) return false;
    if (!Memory::SetHook(true, reinterpret_cast<void**>(&originalUpdate), UpdateHook)) {
        Memory::SetHook(false, reinterpret_cast<void**>(&originalKeyPressed), KeyPressedHook);
        return false;
    }
    return true;
}

bool AgentBridge::SemanticAction(const MapleControl::Action& action, std::string& error) {
    semanticResult = MapleControl::ActionResult{};
    if (action.kind == MapleControl::ActionKind::Stop) {
        semanticResult.dispatched = active;
        semanticResult.reason = "stop_requested";
        ScriptRelease();
        return true;
    }
    unsigned int key = 0;
    unsigned int duration = action.durationMs == 0 ? 60 : action.durationMs;
    switch (action.kind) {
    case MapleControl::ActionKind::Move:
    case MapleControl::ActionKind::Face:
        if (action.direction == "left") key = VK_LEFT;
        else if (action.direction == "right") key = VK_RIGHT;
        else if (action.direction == "up" && action.kind == MapleControl::ActionKind::Move) key = VK_UP;
        else if (action.direction == "down" && action.kind == MapleControl::ActionKind::Move) key = VK_DOWN;
        else { error = "unsupported: direction"; return false; }
        if (action.kind == MapleControl::ActionKind::Face) {
            if (currentState->facingValid && currentState->facingLeft == (key == VK_LEFT)) {
                semanticResult.reason = "already_facing";
                ScriptRelease();
                return true;
            }
            duration = 30;
        }
        break;
    case MapleControl::ActionKind::Jump:
        if (!BindingKey(5, 51, key, error)) return false;
        break;
    case MapleControl::ActionKind::Attack:
        if (!BindingKey(5, 52, key, error)) return false;
        break;
    case MapleControl::ActionKind::Pickup:
        if (!BindingKey(5, 50, key, error)) return false;
        break;
    case MapleControl::ActionKind::UseItem:
        if (!BindingKey(2, action.itemId, key, error)) return false;
        break;
    case MapleControl::ActionKind::Stop:
        break;
    }
    if (!ScriptKeys({key}, duration, error)) return false;
    semanticResult.dispatched = true;
    semanticResult.reason = "native_input_dispatched";
    return true;
}

MapleControl::ActionResult AgentBridge::SemanticResult() {
    auto result = semanticResult;
    result.controlReleased = ScriptReleased();
    return result;
}

bool AgentBridge::ScriptRawKeys(const std::vector<std::string>& names, unsigned int duration,
    std::string& error) {
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
    std::vector<unsigned int> keys;
    for (const auto& name : names) {
        unsigned int key = 0;
        for (const auto& entry : named) if (name == entry.name) { key = entry.code; break; }
        if (key == 0 && name.size() == 1) {
            if (name[0] >= 'a' && name[0] <= 'z') key = name[0] - 'a' + 'A';
            else if ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')) key = name[0];
        }
        if (key == 0 && name.size() >= 2 && name.size() <= 3 && name[0] == 'F' &&
            name[1] >= '1' && name[1] <= '9') {
            unsigned int number = name[1] - '0';
            if (name.size() == 3) {
                if (name[2] < '0' || name[2] > '9') { error = "unsupported: raw_key"; return false; }
                number = number * 10 + name[2] - '0';
            }
            if (number <= 12) key = VK_F1 + number - 1;
        }
        if (key == 0) { error = "unsupported: raw_key"; return false; }
        keys.push_back(key);
    }
    return ScriptKeys(keys, duration, error);
}

bool AgentBridge::ScriptKeys(const std::vector<unsigned int>& keys, unsigned int durationMs, std::string& error) {
    inputOwner = InputOwner::Script;
    return ApplyDesiredKeys(keys, durationMs, error);
}

void AgentBridge::CharacterUpdated(std::uintptr_t player) {
    std::uint32_t current = 0;
    if (ReadAt(0x00BEBF98, current) && current == player) {
        updatedCharacter = current;
        ++characterUpdates;
    }
}

void AgentBridge::ScriptRelease() {
    BeginRelease({}, {});
}

bool AgentBridge::ScriptReleased() {
    return !active && !releasing;
}

bool AgentBridge::ScriptReleasePending() {
    return releasing;
}

bool AgentBridge::InvokeSkill(std::uint32_t skillId, int& result, std::string& error) {
    if (!currentState->mapValid || !currentState->characterValid || !currentState->positionValid ||
        currentState->character.values[6] == 0) {
        error = "state_unavailable_or_dead";
        return false;
    }
    std::uint32_t player = 0;
    if (!ReadAt(0x00BEBF98, player) || player != currentState->user) {
        error = "character_changed";
        return false;
    }
    result = activeSkill(reinterpret_cast<ClientPlayer*>(player), static_cast<int>(skillId), 0, nullptr);
    haveSkillResult = true;
    lastSkillId = skillId;
    lastSkillResult = result;
    return true;
}

bool AgentBridge::ScriptLogs(const std::string& scriptId, const std::vector<std::string>& lines) {
    std::ostringstream records;
    for (const auto& line : lines) records << "{\"script_id\":" << Quote(scriptId)
        << ",\"at_ms\":" << GetTickCount64() << ",\"text\":" << Quote(line) << "}\n";
    const std::string bytes = records.str();
    const HANDLE file = CreateFileA((directory + "\\script-events.jsonl").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
        nullptr) && written == bytes.size();
    CloseHandle(file);
    return complete;
}
