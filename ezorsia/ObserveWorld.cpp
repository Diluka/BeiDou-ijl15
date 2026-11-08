// SPDX-License-Identifier: AGPL-3.0-or-later
#include "stdafx.h"
#include "NativeObservation.h"

#include <array>
#include <cstring>
#include <set>
#include <sstream>

namespace NativeObservation {
namespace {
using Address = std::uint32_t;

std::string Hex(Address address) {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
}

std::string FixedName(Address record) {
    const auto bytes = Bytes(record + 4, 13);
    std::string name;
    for (const auto byte : bytes) {
        if (byte == 0) break;
        name.push_back(static_cast<char>(byte));
    }
    return Utf8(name);
}

Value TemplateName(Address field) {
    try {
        const auto pointer = Read<Address>(field);
        if (pointer == 0) return {};
        const auto length = Read<std::uint32_t>(pointer - 4);
        if (length == 0 || length > 512) return {};
        const auto bytes = Bytes(pointer, length + 1);
        if (bytes.back() != 0) return {};
        return Utf8(std::string(bytes.begin(), bytes.end() - 1));
    }
    catch (const ReadError&) {
        return {};
    }
}

Value Rect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom) {
    return Value::Object({{"left", left}, {"top", top}, {"right", right}, {"bottom", bottom}});
}

Value Monsters() {
    const auto pool = Read<Address>(0x00BEBFA4);
    if (pool == 0 || Read<Address>(pool) != 0x00AF8378)
        throw ReadError("The current monster pool does not match the V17 layout.");
    const auto membership = Read<std::array<Address, 3>>(pool + 0x24);
    const auto count = membership[0];
    if (count > 512) throw ReadError("The active monster count exceeds the observation limit.");
    auto output = Value::Array();
    auto position = membership[1];
    std::set<Address> visited;
    std::uint32_t observed = 0;
    while (position != 0 && observed < count) {
        if (!visited.insert(position).second) throw ReadError("The monster list changed or contains a cycle.");
        const auto monster = Read<Address>(position + 4);
        const auto tables = Read<std::array<Address, 3>>(monster);
        if (tables != std::array<Address, 3>{0x00AF8270, 0x00AF824C, 0x00AF8248})
            throw ReadError("A monster object does not match the V17 layout.");
        const auto coordinates = Read<std::array<std::int32_t, 2>>(monster + 0x510);
        const auto definition = Read<Address>(monster + 0x188);
        output.Add(Value::Object({
            {"address", Hex(monster)}, {"object_id", SecureInt(monster + 0x17C)},
            {"template_id", definition != 0 ? Value(SecureInt(definition + 0x0C)) : Value()},
            {"name", definition != 0 ? TemplateName(definition + 0x30) : Value()},
            {"x", coordinates[0]}, {"y", coordinates[1]},
            {"active", SecureInt(monster + 0x124) != 0}
        }));
        const auto node = Read<Address>(position - 12);
        position = node != 0 ? node + 16 : 0;
        ++observed;
    }
    if (position != 0 || observed != count || Read<Address>(0x00BEBFA4) != pool ||
        Read<std::array<Address, 3>>(pool + 0x24) != membership)
        throw ReadError("The monster pool changed while being observed.");
    return output;
}

Value Drops() {
    const auto pool = Read<Address>(0x00BED6AC);
    if (pool == 0 || Read<Address>(pool) != 0x00AF3700)
        throw ReadError("The current drop pool does not match the V17 layout.");
    const auto membership = Read<std::array<Address, 3>>(pool + 0x28);
    const auto count = membership[0];
    if (count > 512) throw ReadError("The drop count exceeds the observation limit.");
    auto output = Value::Array();
    auto position = membership[1];
    std::set<Address> visited;
    std::uint32_t observed = 0;
    while (position != 0 && observed < count) {
        if (!visited.insert(position).second) throw ReadError("The drop list changed or contains a cycle.");
        const auto drop = Read<Address>(position + 4);
        const bool money = Read<std::uint32_t>(drop + 0x30) != 0;
        const auto value = Read<std::uint32_t>(drop + 0x34);
        output.Add(Value::Object({
            {"address", Hex(drop)}, {"object_id", Read<std::uint32_t>(drop + 0x20)},
            {"item_id", money ? Value() : Value(value)}, {"money", money ? Value(value) : Value()},
            {"x", static_cast<std::int32_t>(SecureValue(drop + 0x74))},
            {"y", static_cast<std::int32_t>(SecureValue(drop + 0x68))},
            {"landed", Read<std::uint32_t>(drop + 0x48) == 3}
        }));
        const auto node = Read<Address>(position - 12);
        position = node != 0 ? node + 16 : 0;
        ++observed;
    }
    if (position != 0 || observed != count || Read<Address>(0x00BED6AC) != pool ||
        Read<std::array<Address, 3>>(pool + 0x28) != membership)
        throw ReadError("The drop pool changed while being observed.");
    return output;
}

Value Npcs() {
    const auto pool = Read<Address>(0x00BED780);
    if (pool == 0 || Read<Address>(pool) != 0x00AFC9F8)
        throw ReadError("The current NPC pool does not match the V17 layout.");
    const auto membership = Read<std::array<Address, 3>>(pool + 0x24);
    const auto count = membership[0];
    if (count > 512) throw ReadError("The NPC count exceeds the observation limit.");
    auto output = Value::Array();
    auto node = membership[1];
    std::set<Address> visited;
    std::uint32_t observed = 0;
    while (node != 0 && observed < count) {
        if (!visited.insert(node).second) throw ReadError("The NPC list changed or contains a cycle.");
        const auto npc = Read<Address>(node + 0x10);
        const auto tables = Read<std::array<Address, 3>>(npc);
        if (tables != std::array<Address, 3>{0x00AFC9D8, 0x00AFC9B4, 0x00AFC9B0})
            throw ReadError("An NPC object does not match the V17 layout.");
        const auto definition = Read<Address>(npc + 0xA8);
        const auto coordinates = Read<std::array<std::int32_t, 2>>(npc + 0x150);
        output.Add(Value::Object({
            {"address", Hex(npc)}, {"object_id", Read<std::uint32_t>(npc + 0xA4)},
            {"template_id", Read<std::uint32_t>(definition)}, {"name", TemplateName(definition + 4)},
            {"x", coordinates[0]}, {"y", coordinates[1]}
        }));
        node = Read<Address>(node + 4);
        ++observed;
    }
    if (node != 0 || observed != count || Read<Address>(0x00BED780) != pool ||
        Read<std::array<Address, 3>>(pool + 0x24) != membership)
        throw ReadError("The NPC pool changed while being observed.");
    return output;
}

std::uint16_t StackQuantity(Address address) {
    const auto data = Read<std::array<unsigned char, 8>>(address);
    std::uint32_t checksum = 0xBAADF00D;
    for (unsigned int index = 0; index < 2; ++index) {
        const std::uint32_t mixed = data[index] ^ checksum;
        checksum = data[index + 2] + ((mixed >> 5) | (mixed << 27));
    }
    std::uint32_t expected = 0;
    std::memcpy(&expected, data.data() + 4, sizeof(expected));
    if (checksum != expected) throw ReadError("An inventory stack quantity failed its native checksum.");
    return static_cast<std::uint16_t>((data[0] ^ data[2]) | ((data[1] ^ data[3]) << 8));
}
}

Value Stage() {
    auto output = Value::Object({
        {"stage", "STARTING"}, {"stage_pointer", {}}, {"stage_vtable", {}},
        {"channel", {}}, {"world_id", {}}, {"character", {}},
        {"login", {}}, {"channel_panel", {}}, {"unavailable", Value::Array()}
    });
    std::string stageName = "STARTING";
    try {
        const auto stage = Read<Address>(0x00BEDED4);
        if (stage != 0) {
            output["stage_pointer"] = Hex(stage);
            const auto table = Read<Address>(stage);
            output["stage_vtable"] = Hex(table);
            if (table == 0x00AF6B24) {
                const auto step = Read<std::uint32_t>(stage + 0x168);
                const auto transition = Read<std::uint32_t>(stage + 0x16C);
                const auto pending = Read<std::uint32_t>(stage + 0x170);
                const auto characters = Read<Address>(stage + 0x194);
                auto login = Value::Object({
                    {"step", step}, {"raw_step", step}, {"raw_transition", transition}, {"raw_request", pending},
                    {"request_pending", pending != 0}, {"transition_pending", transition != 0},
                    {"selected_index", Read<std::int32_t>(stage + 0x190)},
                    {"character_array", characters != 0 ? Value(Hex(characters)) : Value()},
                    {"character_slots", {}}, {"characters", Value::Array()},
                    {"characters_available", false}, {"character_errors", Value::Array()}
                });
                stageName = step == 0 ? "LOGIN" : step == 1 ? "WORLD_SELECT" : step == 2 ? "CHARACTER_SELECT" : "UNKNOWN";
                if (step == 2 && transition == 0 && pending == 0) {
                    try {
                        const auto count = characters != 0 ? Read<std::uint32_t>(characters - 4) : 0;
                        login["character_slots"] = count;
                        if (count > 24) throw ReadError("The login character list exceeds the supported size.");
                        bool complete = true;
                        for (std::uint32_t index = 0; index < count; ++index) {
                            const Address entry = characters + index * 0x2AC;
                            const auto id = Read<std::uint32_t>(entry);
                            if (id == 0) continue;
                            try {
                                login["characters"].Add(Value::Object({{"index", index}, {"id", id}, {"name", FixedName(entry)}}));
                            }
                            catch (const ReadError& error) {
                                complete = false;
                                login["character_errors"].Add(Value::Object({{"index", index}, {"id", id}, {"error", error.what()}}));
                            }
                        }
                        login["characters_available"] = complete;
                        if (!complete) output["unavailable"].Add("A character name could not be decoded using CP936.");
                    }
                    catch (const ReadError& error) {
                        output["unavailable"].Add(std::string("Character list unavailable: ") + error.what());
                    }
                }
                output["login"] = login;
                if (step == 1) {
                    const auto panel = Read<Address>(0x00BEDA58);
                    if (panel != 0 && Read<Address>(panel) == 0x00AF7B4C) {
                        if (Read<std::uint32_t>(panel + 0xB8) != 0) stageName = "TRANSITION";
                        else {
                            stageName = "CHANNEL_SELECT";
                            const auto world = Read<Address>(panel + 0xE0);
                            const auto channels = world != 0 ? Read<Address>(world + 0x1C) : 0;
                            const auto count = channels != 0 ? Read<std::uint32_t>(channels - 4) : 0;
                            if (count > 20) throw ReadError("The channel list exceeds the supported size.");
                            const auto dimensions = Read<std::array<std::int32_t, 2>>(panel + 0x24);
                            const auto layer = Read<Address>(panel + 0x18);
                            const auto renderer = Read<Address>(0x00BF14EC);
                            bool viewportKnown = false;
                            bool originKnown = false;
                            std::array<std::int32_t, 4> viewport{};
                            std::int32_t originX = 0;
                            std::int32_t originY = 0;
                            if (renderer != 0 && Read<Address>(renderer) == 0x5042D154) {
                                viewport = Read<std::array<std::int32_t, 4>>(renderer + 0xF8);
                                const auto size = Read<std::array<std::int32_t, 2>>(renderer + 0x20);
                                const auto center = Read<std::array<std::int32_t, 2>>(renderer + 0xC4);
                                viewportKnown = viewport[2] - viewport[0] == size[0] &&
                                    viewport[3] - viewport[1] == size[1] &&
                                    viewport[0] == center[0] - size[0] / 2 && viewport[1] == center[1] - size[1] / 2;
                            }
                            if (viewportKnown && layer != 0 && Read<Address>(layer) == 0x5042E3F0) {
                                const auto rect = Read<std::array<std::int32_t, 4>>(layer + 0x54);
                                originKnown = rect[2] - rect[0] == dimensions[0] && rect[3] - rect[1] == dimensions[1];
                                originX = rect[0] - viewport[0];
                                originY = rect[1] - viewport[1];
                            }
                            auto buttons = Value::Array();
                            for (std::uint32_t index = 0; index < count; ++index) {
                                const auto left = 35 + 94 * static_cast<std::int32_t>(index % 4);
                                const auto top = 57 + 32 * static_cast<std::int32_t>(index / 4);
                                Value gameRect;
                                if (originKnown && left + 92 <= dimensions[0] && top + 30 <= dimensions[1])
                                    gameRect = Rect(originX + left, originY + top, originX + left + 92, originY + top + 30);
                                buttons.Add(Value::Object({{"index", index}, {"channel", index + 1},
                                    {"game_rect", gameRect}, {"root_rect", {}}}));
                            }
                            output["channel_panel"] = Value::Object({
                                {"selected_index", Read<std::int32_t>(panel + 0xDC)}, {"count", count},
                                {"camera_viewport", viewportKnown ? Rect(viewport[0], viewport[1], viewport[2], viewport[3]) : Value()},
                                {"game_origin", originKnown ? Value::Object({{"x", originX}, {"y", originY}}) : Value()},
                                {"channels", buttons},
                                {"layout_validation", "static-hit-test-0x6299E2-camera-projection-click-pending"}
                            });
                        }
                    }
                }
                if (transition != 0) stageName = "TRANSITION";
            }
            else {
                const auto user = Read<Address>(0x00BEBF98);
                stageName = "TRANSITION";
                if (user != 0 && Read<std::array<Address, 2>>(user) == std::array<Address, 2>{0x00B3D20C, 0x00B3D1E8})
                    stageName = "IN_GAME";
            }
            const auto context = Read<Address>(0x00BE7918);
            if (context != 0) {
                const auto world = Read<std::int32_t>(context + 0x2054);
                const auto channel = Read<std::int32_t>(context + 0x2058);
                output["world_id"] = world;
                if (channel >= 0 && channel < 20) output["channel"] = channel + 1;
                const auto stats = Read<Address>(context + 0x20B8);
                if (stageName == "IN_GAME" && stats != 0)
                    output["character"] = Value::Object({{"id", Read<std::uint32_t>(stats)}, {"name", FixedName(stats)}});
            }
            if (Read<Address>(0x00BEDED4) != stage) {
                stageName = "TRANSITION";
                output["login"] = {};
                output["character"] = {};
                output["channel_panel"] = {};
                output["unavailable"].Add("The current stage changed during observation.");
            }
        }
    }
    catch (const ReadError& error) {
        stageName = "UNKNOWN";
        output["unavailable"].Add(error.what());
    }
    output["stage"] = stageName;
    return output;
}

Value Scene() {
    const auto field = Read<Address>(0x00BEDED4);
    const auto user = Read<Address>(0x00BEBF98);
    if (field == 0 || user == 0 || Read<std::array<Address, 2>>(user) != std::array<Address, 2>{0x00B3D20C, 0x00B3D1E8})
        throw ReadError("The current game scene has no verified local character.");
    const auto coordinates = Read<std::array<std::int32_t, 2>>(user + 0x116C);
    const auto flags = Read<std::uint32_t>(user + 0x570);
    auto output = Value::Object({
        {"source", "client-native-api"}, {"readonly", true}, {"map_id", SecureValue(field + 0x138)},
        {"player", Value::Object({{"x", coordinates[0]}, {"y", coordinates[1]}})},
        {"monsters", Monsters()}, {"drops", Drops()}, {"npcs", Npcs()},
        {"player_facing", flags & 1 ? "Left" : "Right"}, {"raw_flags", flags}, {"pending_dynamic", false}
    });
    if (Read<Address>(0x00BEDED4) != field || Read<Address>(0x00BEBF98) != user)
        throw ReadError("The current scene changed during observation.");
    return output;
}

Value Inventory() {
    auto output = Value::Object({
        {"source", "client-native-api"}, {"readonly", true}, {"items", Value::Array()},
        {"equipped", Value::Array()}, {"capacities", Value::Object()}, {"checksums_valid", true},
        {"unavailable", Value::Array()},
        {"not_covered", Value::Array({"item_names", "quest_status", "cash_equipped_items"})}
    });
    const auto context = Read<Address>(0x00BE7918);
    const auto character = context != 0 ? Read<Address>(context + 0x20B8) : 0;
    if (character == 0) {
        output["checksums_valid"] = false;
        output["unavailable"].Add("The current character inventory is unavailable during a scene change.");
        return output;
    }
    for (int slot = -1; slot >= -51; --slot) {
        const Address pointer = character + 0xEB - slot * 8;
        const auto item = Read<Address>(pointer);
        if (item == 0) continue;
        try {
            if (Read<Address>(item) != 0x00AF310C) throw ReadError("An equipped item type is not covered by the V17 layout.");
            const auto identifier = SecureValue(item + 0x0C);
            if (Read<Address>(pointer) != item) throw ReadError("An equipped slot reference changed during observation.");
            output["equipped"].Add(Value::Object({{"inventory_type", 1}, {"slot", slot}, {"item_id", identifier}, {"quantity", 1}}));
        }
        catch (const ReadError& error) {
            output["checksums_valid"] = false;
            output["unavailable"].Add(std::string("Equipped slot ") + std::to_string(slot) + ": " + error.what());
        }
    }
    for (int type = 1; type <= 5; ++type) {
        const auto array = Read<Address>(character + 0x447 + type * 4);
        if (array == 0) {
            output["unavailable"].Add(std::string("Inventory array unavailable: ") + std::to_string(type));
            continue;
        }
        const auto count = Read<std::uint32_t>(array - 4);
        if (count == 0 || count > 257) throw ReadError("An inventory slot array exceeds the supported size.");
        const auto slots = Bytes(array, count * 8);
        output["capacities"][std::to_string(type)] = count - 1;
        std::vector<Value> current;
        for (std::uint32_t slot = 1; slot < count; ++slot) {
            Address item = 0;
            std::memcpy(&item, slots.data() + slot * 8 + 4, sizeof(item));
            if (item == 0) continue;
            const auto table = Read<Address>(item);
            if (table != 0x00AF310C && table != 0x00AF318C && table != 0x00AF3204) {
                output["unavailable"].Add(std::string("Inventory item type not covered: ") + std::to_string(type) + "/" + std::to_string(slot));
                continue;
            }
            try {
                const auto identifier = SecureValue(item + 0x0C);
                Value quantity;
                if (table == 0x00AF310C) quantity = 1;
                else if (table == 0x00AF318C) quantity = StackQuantity(item + 0x28);
                current.push_back(Value::Object({{"inventory_type", type}, {"slot", slot},
                    {"item_id", identifier}, {"quantity", quantity}}));
            }
            catch (const ReadError& error) {
                output["checksums_valid"] = false;
                output["unavailable"].Add(std::string("Inventory slot ") + std::to_string(type) + "/" + std::to_string(slot) + ": " + error.what());
            }
        }
        if (Read<Address>(character + 0x447 + type * 4) != array || Bytes(array, count * 8) != slots)
            output["unavailable"].Add(std::string("Inventory array changed during observation: ") + std::to_string(type));
        else for (auto& item : current) output["items"].Add(std::move(item));
    }
    if (Read<Address>(context + 0x20B8) != character) {
        output["items"] = Value::Array();
        output["equipped"] = Value::Array();
        output["unavailable"].Add("The current character changed during inventory observation.");
    }
    return output;
}
}
