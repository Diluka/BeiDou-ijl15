// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace NativeObservation {

// Small JSON output value: observations never parse arbitrary JSON in the game thread.
class Value {
public:
    enum class Kind { Null, Boolean, Number, String, Array, Object };
    Value() = default;
    Value(bool value) : kind(Kind::Boolean), boolean(value) {}
    Value(const char* value) : kind(Kind::String), text(value) {}
    Value(const std::string& value) : kind(Kind::String), text(value) {}
    Value(double value) : kind(Kind::Number), number(value) {}
    template<class T, typename std::enable_if<std::is_integral<T>::value &&
        !std::is_same<T, bool>::value, int>::type = 0>
    Value(T value) : kind(Kind::Number), number(static_cast<double>(value)) {}
    static Value Object(std::initializer_list<std::pair<std::string, Value>> values = {});
    static Value Array(std::initializer_list<Value> values = {});
    Value& operator[](const std::string& key);
    void Add(Value value);
    std::string Dump() const;
private:
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;
};

class ReadError : public std::runtime_error {
public:
    explicit ReadError(const std::string& message) : std::runtime_error(message) {}
};
void Copy(std::uintptr_t address, void* output, std::size_t size);
template<class T> T Read(std::uintptr_t address) {
    T value{};
    Copy(address, &value, sizeof(value));
    return value;
}
std::vector<unsigned char> Bytes(std::uintptr_t address, std::size_t size);
std::string Utf8(const std::string& cp936);
std::string ZxString(std::uintptr_t field, std::size_t maximum = 65536);
std::uint32_t SecureInt(std::uintptr_t address);
std::uint32_t SecureValue(std::uintptr_t address);

Value Stage();
Value Scene();
Value Inventory();
Value Character();
Value Quests();
Value Hotkeys();
Value Dialog();
Value Window(std::uintptr_t address);
Value Ui();
Value Shop();

// Separate read-only request lane; it never replaces a running action/script request.
void Tick(const std::string& directory, const std::string& instance,
    const std::string& run, std::uint64_t generation, std::uint64_t sequence);
}
