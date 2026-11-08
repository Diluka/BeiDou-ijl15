// SPDX-License-Identifier: AGPL-3.0-or-later
#include "stdafx.h"
#include "NativeObservation.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace NativeObservation {
namespace {
std::string Quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') out << '\\' << byte;
        else if (byte < 32) out << "\\u00" << std::hex << std::setfill('0')
            << std::setw(2) << static_cast<unsigned int>(byte) << std::dec;
        else out << byte;
    }
    out << '"';
    return out.str();
}

std::uint64_t UnixMs() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return (ticks - 116444736000000000ULL) / 10000;
}
}

Value Value::Object(std::initializer_list<std::pair<std::string, Value>> values) {
    Value result;
    result.kind = Kind::Object;
    result.object = values;
    return result;
}

Value Value::Array(std::initializer_list<Value> values) {
    Value result;
    result.kind = Kind::Array;
    result.array = values;
    return result;
}

Value& Value::operator[](const std::string& key) {
    if (kind == Kind::Null) kind = Kind::Object;
    for (auto& entry : object) if (entry.first == key) return entry.second;
    object.emplace_back(key, Value{});
    return object.back().second;
}

void Value::Add(Value value) {
    if (kind == Kind::Null) kind = Kind::Array;
    array.push_back(std::move(value));
}

std::string Value::Dump() const {
    switch (kind) {
    case Kind::Null: return "null";
    case Kind::Boolean: return boolean ? "true" : "false";
    case Kind::String: return Quote(text);
    case Kind::Number: {
        if (!std::isfinite(number)) return "null";
        std::ostringstream out;
        out << std::setprecision(17) << number;
        return out.str();
    }
    case Kind::Array: {
        std::string out = "[";
        bool separator = false;
        for (const auto& entry : array) {
            if (separator) out += ',';
            out += entry.Dump();
            separator = true;
        }
        return out + ']';
    }
    case Kind::Object: {
        std::string out = "{";
        bool separator = false;
        for (const auto& entry : object) {
            if (separator) out += ',';
            out += Quote(entry.first) + ':' + entry.second.Dump();
            separator = true;
        }
        return out + '}';
    }
    }
    return "null";
}

void Copy(std::uintptr_t address, void* output, std::size_t size) {
    if (size == 0) return;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        output, size, &copied) || copied != size) throw ReadError("native object read unavailable");
}

std::vector<unsigned char> Bytes(std::uintptr_t address, std::size_t size) {
    std::vector<unsigned char> result(size);
    Copy(address, result.data(), size);
    return result;
}

std::string Utf8(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) throw ReadError("native text is not valid CP936");
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &wide[0], count);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    if (bytes != 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, &result[0], bytes, nullptr, nullptr);
    return result;
}

std::string ZxString(std::uintptr_t field, std::size_t maximum) {
    const auto pointer = Read<std::uint32_t>(field);
    if (pointer == 0) return {};
    const auto length = Read<std::uint32_t>(pointer - 4);
    if (length > maximum) throw ReadError("native string exceeds observation boundary");
    const auto bytes = Bytes(pointer, length + 1);
    if (bytes[length] != 0) throw ReadError("native string terminator unavailable");
    return Utf8(std::string(reinterpret_cast<const char*>(bytes.data()), length));
}

std::uint32_t SecureInt(std::uintptr_t address) {
    const auto data = Read<std::array<std::uint32_t, 3>>(address);
    const auto mixed = data[0] ^ 0xBAADF00D;
    const auto checksum = data[1] + ((mixed >> 5) | (mixed << 27));
    if (checksum != data[2]) throw ReadError("native integer checksum unavailable");
    return data[0] ^ ((data[1] << 5) | (data[1] >> 27));
}

std::uint32_t SecureValue(std::uintptr_t address) {
    const auto guards = Read<std::array<unsigned char, 12>>(address);
    std::uint32_t pointer = 0;
    std::memcpy(&pointer, guards.data() + 8, sizeof(pointer));
    const auto encoded = Read<std::array<unsigned char, 10>>(pointer);
    unsigned char key = encoded[4];
    std::uint16_t checksum = 0x9A65;
    std::uint32_t result = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        if (key == 0) key = 42;
        result |= static_cast<std::uint32_t>(encoded[index] ^ key) << (index * 8);
        key = static_cast<unsigned char>(key + encoded[index] + 42);
        checksum = static_cast<std::uint16_t>((checksum << 3) | ((checksum >> 13) + key));
    }
    std::uint16_t expected = 0;
    std::memcpy(&expected, encoded.data() + 8, sizeof(expected));
    if (guards[0] != encoded[5] || guards[4] != encoded[6] || checksum != expected)
        throw ReadError("native value checksum unavailable");
    return result;
}

void Tick(const std::string& directory, const std::string& instance,
    const std::string& run, std::uint64_t generation, std::uint64_t sequence) {
    const std::string path = directory + "\\observation-request.txt";
    const HANDLE input = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) return;
    std::array<char, 513> bytes{};
    DWORD length = 0;
    const bool complete = ReadFile(input, bytes.data(), 512, &length, nullptr) != FALSE;
    CloseHandle(input);
    DeleteFileA(path.c_str());
    std::istringstream request(std::string(bytes.data(), length));
    int protocol = 0;
    std::string target, id, domain, extra;
    if (!complete || length == 512 || !(request >> protocol >> target >> id >> domain) ||
        request >> extra || protocol != 1 || target != instance || id.size() != 32 ||
        id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return;
    using Collector = Value (*)();
    const std::pair<const char*, Collector> collectors[]{
        {"stage", Stage}, {"scene", Scene}, {"inventory", Inventory}, {"character", Character},
        {"quests", Quests}, {"hotkeys", Hotkeys}, {"dialog", Dialog}, {"ui", Ui}, {"shop", Shop}
    };
    Collector selected = nullptr;
    for (const auto& entry : collectors) if (domain == entry.first) selected = entry.second;
    Value data;
    std::string status = selected == nullptr ? "unsupported" : "ok";
    std::string error;
    if (selected != nullptr) {
        try { data = selected(); }
        catch (const ReadError& failure) { status = "unavailable"; error = failure.what(); }
    }
    else error = "unsupported observation domain";
    const auto output = Value::Object({{"protocol", 1}, {"instance_id", instance}, {"run_id", run},
        {"request_id", id}, {"domain", domain}, {"seq", sequence}, {"scene_generation", generation},
        {"observed_at_ms", UnixMs()}, {"status", status}, {"data", data},
        {"error", error.empty() ? Value{} : Value(error)}}).Dump();
    const std::string temporary = directory + "\\observation.tmp";
    const HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool saved = WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written,
        nullptr) && written == output.size();
    CloseHandle(file);
    if (saved) MoveFileExA(temporary.c_str(), (directory + "\\observation.json").c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}
