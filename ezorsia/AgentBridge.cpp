#include "stdafx.h"
#include "AgentBridge.h"

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

auto originalUpdate = reinterpret_cast<ApplicationUpdate>(0x009F84D0);
auto originalKeyPressed = reinterpret_cast<KeyPressed>(0x0059A25A);
const auto dispatchInput = reinterpret_cast<InputMessage>(0x009F97B7);
const auto inputModifiers = reinterpret_cast<InputModifiers>(0x0059B1EB);

struct CharacterState {
    std::uint32_t id = 0;
    std::string name;
    std::array<std::uint32_t, 13> values{};
};

struct GameState {
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
};

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
bool active = false;
bool releasing = false;
bool haveScene = false;
GameState previousState;
std::uint64_t sceneGeneration = 1;
std::uint64_t sequence = 0;
std::uint64_t inputEvents = 0;
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

bool SecureMap(std::uint32_t field, std::uint32_t& map) {
    std::array<unsigned char, 12> guards{};
    if (!ReadAt(field + 0x138, guards)) return false;
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

GameState CaptureState() {
    GameState state;
    ReadAt(0x00BEDED4, state.field);
    if (state.field != 0) state.mapValid = SecureMap(state.field, state.map);
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
        }
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
    for (unsigned int key = 0; key < held.size(); ++key) if (held[key]) {
        held[key] = false;
        SendKey(key, false);
    }
    active = false;
    releasing = true;
    pendingStatus = status;
    pendingReason = reason;
}

bool Publish(const GameState& state) {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const std::uint64_t ticks = (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) |
        fileTime.dwLowDateTime;
    const std::uint64_t unixMs = (ticks - 116444736000000000ULL) / 10000;
    const bool ready = state.mapValid && state.characterValid && state.positionValid;
    std::ostringstream out;
    out << "{\"protocol\":1,\"instance_id\":" << Quote(instanceId)
        << ",\"pid\":" << GetCurrentProcessId() << ",\"run_id\":" << Quote(runId)
        << ",\"seq\":" << ++sequence << ",\"observed_at_ms\":" << unixMs
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
    const GameState state = CaptureState();
    const bool changed = haveScene && (state.field != previousState.field ||
        state.stats != previousState.stats || state.mapValid != previousState.mapValid ||
        (state.mapValid && state.map != previousState.map) ||
        (state.characterValid && state.character.id != previousState.character.id));
    if (changed) ++sceneGeneration;
    previousState = state;
    haveScene = true;

    // A release is complete only after the original game update has observed the cleared keys.
    if (releasing) {
        releasing = false;
        owned.fill(false);
        commandStatus = pendingStatus;
        commandReason = pendingReason;
    }
    const bool ready = state.mapValid && state.characterValid && state.positionValid;
    if (active && (changed || !ready || state.character.values[6] == 0))
        BeginRelease("cancelled", changed ? "scene_changed" : !ready ? "state_unavailable" : "dead");
    else if (active && now >= deadline) BeginRelease("completed", "duration_elapsed");

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
        unsigned int duration = 0;
        std::string targetInstance;
        std::string id;
        std::string operation;
        std::istringstream command(std::string(buffer.data(), bytes));
        const bool header = read && bytes < 1024 &&
            static_cast<bool>(command >> protocol >> targetInstance >> generation >> id >> operation >> duration);
        if (!header) {
            rejectedId = id;
            rejectedReason = "invalid_command";
        }
        else if (id != lastConsumedId) {
            lastConsumedId = id;
            rejectedId.clear();
            rejectedReason.clear();
            std::vector<unsigned int> keys;
            unsigned int key = 0;
            while (command >> key) keys.push_back(key);
            bool valid = command.eof() && protocol == 1 && targetInstance == instanceId &&
                !id.empty() && id.size() <= 64 && id.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos;
            std::uint32_t input = 0;
            if (operation == "keys") {
                valid = valid && generation == sceneGeneration && duration >= 1 && duration <= 30000 &&
                    !keys.empty() && keys.size() <= 16 && ready && state.character.values[6] != 0 &&
                    ReadAt(0x00BEC33C, input) && input != 0;
                for (unsigned int candidate : keys) {
                    std::uint32_t scan = 0;
                    if (candidate > 255 || candidate == 0 ||
                        !ReadAt(input + 0x24 + candidate * 4, scan) || scan == 0 || scan > 255) valid = false;
                    else scanCodes[candidate] = scan;
                }
            }
            else valid = valid && operation == "stop" && duration == 0 && keys.empty();

            if (!valid || (operation == "keys" && (active || releasing))) {
                // Callers serialize mutations with maple's input lock. A rejected request never presses keys.
                rejectedId = id;
                rejectedReason = valid ? "busy" : "invalid_or_stale_command";
                if (!active && !releasing) {
                    commandId = id;
                    commandStatus = "rejected";
                    commandReason = rejectedReason;
                }
            }
            else {
                commandId = id;
                commandStatus = "running";
                commandReason.clear();
                if (operation == "stop") BeginRelease("completed", "stopped");
                else {
                    deadline = now + duration;
                    active = true;
                    for (unsigned int candidate : keys) {
                        if (held[candidate]) continue;
                        owned[candidate] = true;
                        held[candidate] = true;
                        if (!SendKey(candidate, true)) {
                            BeginRelease("cancelled", "input_route_unavailable");
                            break;
                        }
                    }
                }
            }
        }
    }
    if (now - lastPublishedAt >= 25) {
        if (!Publish(state) && active) BeginRelease("cancelled", "state_publish_failed");
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
