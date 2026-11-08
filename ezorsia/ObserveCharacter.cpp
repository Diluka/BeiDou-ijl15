// SPDX-License-Identifier: AGPL-3.0-or-later
#include "stdafx.h"
#include "NativeObservation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace NativeObservation {
namespace {
using Address = std::uint32_t;

template<class T> T At(const std::vector<unsigned char>& data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

Address U32(std::uintptr_t address) { return Read<Address>(address); }
std::int32_t I32(std::uintptr_t address) { return Read<std::int32_t>(address); }

std::string Hex(std::uintptr_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

Address CurrentCharacter() {
    const auto context = U32(0x00BE7918);
    if (context == 0) throw ReadError("CWvsContext is unavailable");
    const auto character = U32(context + 0x20B8);
    if (character == 0) throw ReadError("CharacterData is unavailable");
    return character;
}

struct MapNode {
    Address address;
    std::vector<unsigned char> bytes;
};

// These packed ZMaps are the actual v83 client records, not a second state model.
std::vector<MapNode> MapRecords(std::uintptr_t address, Address expectedTable,
    Address expectedNode, std::size_t nodeSize) {
    const auto header = Bytes(address, 24);
    const auto table = At<Address>(header, 4);
    const auto buckets = At<Address>(header, 8);
    const auto count = At<Address>(header, 12);
    if (At<Address>(header, 0) != expectedTable || buckets == 0 || buckets > 65536 || count > 65536)
        throw ReadError("ZMap layout or record bounds do not match V17");
    if (count != 0 && table == 0) throw ReadError("nonempty ZMap has no bucket array");
    const auto pointers = table != 0 ? Bytes(table, buckets * 4) : std::vector<unsigned char>{};
    std::set<Address> visited;
    std::vector<MapNode> nodes;
    nodes.reserve(count);
    for (std::size_t bucket = 0; bucket < pointers.size() / 4; ++bucket) {
        auto pointer = At<Address>(pointers, bucket * 4);
        while (pointer != 0) {
            if (visited.size() >= count || !visited.insert(pointer).second)
                throw ReadError("ZMap chain repeats a node or exceeds its declared count");
            auto bytes = Bytes(pointer, nodeSize);
            if (At<Address>(bytes, 0) != expectedNode) throw ReadError("ZMap node type does not match V17");
            const auto next = At<Address>(bytes, 4);
            nodes.push_back({pointer, std::move(bytes)});
            pointer = next;
        }
    }
    if (visited.size() != count || Bytes(address, 24) != header ||
        (table != 0 && Bytes(table, pointers.size()) != pointers))
        throw ReadError("ZMap record set changed during the observation");
    return nodes;
}

Value StringArray(std::initializer_list<const char*> text) {
    auto values = Value::Array();
    for (const auto* value : text) values.Add(value);
    return values;
}

const char* KeyName(unsigned int code) {
    static const std::map<unsigned int, const char*> named{
        {1,"Escape"}, {12,"minus"}, {13,"equal"}, {14,"BackSpace"}, {15,"Tab"},
        {26,"bracketleft"}, {27,"bracketright"}, {28,"Return"}, {29,"Control_L"},
        {39,"semicolon"}, {40,"apostrophe"}, {41,"grave"}, {42,"Shift_L"},
        {43,"backslash"}, {51,"comma"}, {52,"period"}, {53,"slash"}, {54,"Shift_R"},
        {56,"Alt_L"}, {57,"space"}, {71,"Home"}, {72,"Up"}, {73,"Prior"},
        {75,"Left"}, {77,"Right"}, {79,"End"}, {80,"Down"}, {81,"Next"},
        {82,"Insert"}, {83,"Delete"}, {87,"F11"}, {88,"F12"}
    };
    const auto found = named.find(code);
    if (found != named.end()) return found->second;
    static const char* numbers[]{"1","2","3","4","5","6","7","8","9","0"};
    static const char* qwerty[]{"q","w","e","r","t","y","u","i","o","p"};
    static const char* home[]{"a","s","d","f","g","h","j","k","l"};
    static const char* bottom[]{"z","x","c","v","b","n","m"};
    static const char* function[]{"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10"};
    if (code >= 2 && code < 12) return numbers[code - 2];
    if (code >= 16 && code < 26) return qwerty[code - 16];
    if (code >= 30 && code < 39) return home[code - 30];
    if (code >= 44 && code < 51) return bottom[code - 44];
    if (code >= 59 && code < 69) return function[code - 59];
    return nullptr;
}
Value KeyValue(unsigned int code) {
    const auto* name = KeyName(code);
    return name == nullptr ? Value{} : Value(name);
}
}

Value Character() {
    auto unavailable = Value::Array();
    auto result = Value::Object({
        {"source", "client-native-observation"}, {"readonly", true},
        {"mesos", Value{}}, {"mesos_checksum_valid", false}, {"skills", Value::Object()},
        {"skills_complete", false}, {"read_seconds", 0},
        {"not_covered", StringArray({"skill_names", "skill_requirements", "skill_effects",
            "skill_expirations", "master_level_when_absent_from_client_records"})}
    });
    const auto character = CurrentCharacter();
    result["character_id"] = U32(character);
    try {
        result["mesos"] = SecureInt(character + 0xA5);
        result["mesos_checksum_valid"] = true;
    }
    catch (const ReadError& error) { unavailable.Add(error.what()); }

    const auto levels = MapRecords(character + 0x467, 0xAF2074, 0xAF3314, 16);
    const auto masters = MapRecords(character + 0x497, 0xAF2074, 0xAF3314, 16);
    std::map<Address, Address> levelValues;
    std::map<Address, Address> masterValues;
    for (const auto& record : levels) {
        if (!levelValues.emplace(At<Address>(record.bytes, 8), At<Address>(record.bytes, 12)).second)
            throw ReadError("skill level records contain a duplicate ID");
    }
    for (const auto& record : masters) {
        if (!masterValues.emplace(At<Address>(record.bytes, 8), At<Address>(record.bytes, 12)).second)
            throw ReadError("skill master records contain a duplicate ID");
    }
    auto skills = Value::Object();
    for (const auto& record : levelValues) {
        const auto master = masterValues.find(record.first);
        skills[std::to_string(record.first)] = Value::Object({
            {"level", record.second},
            {"master_level", master == masterValues.end() ? Value{} : Value(master->second)}
        });
    }
    result["skills"] = std::move(skills);
    result["skills_complete"] = true;
    result["unavailable"] = std::move(unavailable);
    return result;
}

Value Quests() {
    auto result = Value::Object({
        {"source", "client-native-observation"}, {"readonly", true},
        {"unavailable", Value::Array()}, {"read_seconds", 0},
        {"not_covered", StringArray({"quest_names", "quest_requirements",
            "progress_string_semantics", "available_quests"})}
    });
    const auto character = CurrentCharacter();
    std::map<unsigned int, Value> records;
    for (const bool started : {true, false}) {
        const auto nodes = MapRecords(character + (started ? 0x5FF : 0x4DF),
            started ? 0xAF204C : 0xAF2068, started ? 0xAF3344 : 0xAF332C, 20);
        for (const auto& node : nodes) {
            const auto id = At<std::uint16_t>(node.bytes, 8);
            auto progress = started ? Value(ZxString(node.address + 12, 4096)) : Value{};
            records.emplace(id, Value::Object({{"quest_id", id},
                {"state", started ? "started" : "completed"}, {"progress", std::move(progress)}}));
        }
    }
    auto quests = Value::Array();
    for (auto& record : records) quests.Add(std::move(record.second));
    result["quests"] = std::move(quests);
    return result;
}

Value Hotkeys() {
    const auto character = CurrentCharacter();
    const auto mapped = U32(0x00BED5A0);
    const auto quick = U32(0x00BED658);
    if (mapped == 0 || U32(mapped) != 0xAF5650)
        throw ReadError("CFuncKeyMappedMan does not match V17");
    if (quick == 0 || U32(quick) != 0xAFDA18)
        throw ReadError("CQuickslotKeyMappedMan does not match V17");
    const auto positionCode = Bytes(0x008DE93D, 39);
    const std::array<unsigned char, 6> prefix{{0x8B,0x44,0x24,0x04,0x83,0xF8}};
    if (!std::equal(prefix.begin(), prefix.end(), positionCode.begin()) ||
        positionCode[16] != 0x8B || positionCode[17] != 0x88 ||
        positionCode[24] != 0x8B || positionCode[25] != 0x80)
        throw ReadError("quick-slot position code does not match V17");
    const auto count = positionCode[6];
    if (count != 8 && count != 26) throw ReadError("unsupported quick-slot count");
    const auto xArray = U32(0x008DE94F);
    if (U32(0x008DE957) != xArray + 4) throw ReadError("quick-slot coordinate arrays do not match V17");
    const auto hitCode = Bytes(0x008DE8E5, 20);
    if (hitCode[0] != 0x8D || hitCode[1] != 0xB8 || hitCode[9] != 0x8D || hitCode[10] != 0x98)
        throw ReadError("quick-slot hit-test code does not match V17");
    const auto originX = -At<std::int32_t>(hitCode, 2);
    const auto originY = -At<std::int32_t>(hitCode, 11);
    const auto hitArray = U32(0x008DE8F5) - 4;
    const auto status = U32(0x00BEC208);
    if (status == 0 || U32(status) != 0xB3BA80) throw ReadError("CUIStatusBar does not match V17");
    const auto manager = U32(0x00BEC20C);
    if (manager == 0 || U32(manager) != 0xB3F13C) throw ReadError("CWndMan does not match V17");
    auto dragObject = Value{};
    const auto dragging = U32(manager + 0x98);
    if (dragging != 0) {
        const auto table = U32(dragging);
        dragObject = Value::Object({{"address", Hex(dragging)}, {"vtable", Hex(table)},
            {"inventory_type", Value{}}, {"slot", Value{}}});
        if (table == 0xAF34D8) {
            dragObject["inventory_type"] = U32(dragging + 0x18);
            dragObject["slot"] = U32(dragging + 0x1C);
        }
    }
    const auto table = Bytes(mapped + 4, 89 * 5);
    const auto quickKeys = Bytes(quick + 4, count * 4);
    const auto positions = Bytes(hitArray, count * 8);
    auto bindings = Value::Array();
    for (unsigned int code = 0; code < 89; ++code) {
        bindings.Add(Value::Object({{"key_code", code}, {"key", KeyValue(code)},
            {"type", table[code * 5]}, {"id", At<Address>(table, code * 5 + 1)}}));
    }
    auto quickSlots = Value::Array();
    for (unsigned int index = 0; index < count; ++index) {
        const auto code = At<Address>(quickKeys, index * 4);
        quickSlots.Add(Value::Object({{"index", index}, {"key_code", code}, {"key", KeyValue(code)},
            {"local_x", At<std::int32_t>(positions, index * 8)},
            {"local_y", At<std::int32_t>(positions, index * 8 + 4)}}));
    }
    return Value::Object({{"source", "client-native-observation"}, {"readonly", true},
        {"character_id", U32(character)}, {"bindings", std::move(bindings)},
        {"quickslots", std::move(quickSlots)}, {"origin_x", originX}, {"origin_y", originY},
        {"quickslots_open", U32(status + 0xD10) != 0}, {"drag_object", std::move(dragObject)},
        {"unavailable", Value::Array()},
        {"not_covered", StringArray({"keyboard_editor", "skill_effect_confirmation"})}});
}

Value Shop() {
    auto unavailable = Value::Array();
    auto result = Value::Object({
        {"source", "client-native-observation"}, {"readonly", true}, {"open", false},
        {"window", Value{}}, {"npc_id", Value{}}, {"buy_items", Value::Array()},
        {"sell_items", Value::Array()}, {"buy_first_index", Value{}},
        {"sell_first_index", Value{}}, {"buy_selected_index", Value{}},
        {"sell_selected_index", Value{}}, {"busy", Value{}}, {"quantity_dialog", Value{}},
        {"sell_tab_index", Value{}}, {"sell_tabs", Value::Array()},
        {"not_covered", StringArray({"cash_shop", "unverified_shop_list_layout", "quantity_dialog"})}
    });

    const auto membership = Bytes(0x00BF1650, 12);
    const auto count = At<Address>(membership, 0);
    if (count > 256) throw ReadError("active window count exceeds the V17 observation bound");
    auto node = At<Address>(membership, 4);
    std::vector<Address> shops;
    std::vector<Address> prompts;
    std::set<Address> visited;
    for (Address index = 0; index < count; ++index) {
        if (node == 0 || !visited.insert(node).second)
            throw ReadError("active window list ended early or repeats a node");
        const auto address = U32(node);
        const auto table = U32(address);
        if (table == 0xAF0AF8 || table == 0xAFE110) shops.push_back(address);
        if (table == 0xB3E100 && U32(address + 0x7C) == 2) prompts.push_back(address);
        const auto next = U32(node - 12);
        node = next == 0 ? 0 : next + 16;
    }
    if (Bytes(0x00BF1650, 12) != membership)
        throw ReadError("active window membership changed during shop observation");
    result["open"] = !shops.empty();
    if (shops.size() != 1) {
        if (shops.size() > 1) unavailable.Add("multiple ordinary shop windows are present");
        result["unavailable"] = std::move(unavailable);
        return result;
    }

    const auto address = shops.front();
    result["window"] = Window(address);
    if (U32(address) != 0xAFE110) {
        unavailable.Add("this ordinary shop window list layout has not been calibrated");
        result["unavailable"] = std::move(unavailable);
        return result;
    }
    const auto header = Bytes(address, 0x100);
    const auto tabControl = At<Address>(header, 0x9C);
    const auto tab = U32(tabControl + 0x3C);
    if (tab > 1) throw ReadError("ordinary shop purchase tab is not supported");
    const auto context = U32(0x00BE7918);
    if (context == 0) throw ReadError("shop CWvsContext is unavailable");
    const auto discount = U32(context + 0x355C);
    const auto sellTab = At<Address>(header, 0xA4);
    if (sellTab == 0 || U32(sellTab) != 0xAF2FF8)
        throw ReadError("shop sell-tab control does not match V17");
    const auto tabCount = U32(sellTab + 0x48);
    auto tabNode = U32(sellTab + 0x4C);
    if (tabCount != 5) throw ReadError("ordinary shop sell-tab count does not match V17");
    auto sellTabs = Value::Array();
    for (Address index = 0; index < tabCount; ++index) {
        if (tabNode == 0) throw ReadError("shop sell-tab list ended early");
        sellTabs.Add(Value::Object({{"index", index}, {"left", I32(tabNode + 4)},
            {"right", I32(tabNode + 8)}}));
        const auto next = U32(tabNode - 12);
        tabNode = next == 0 ? 0 : next + 16;
    }
    result["npc_id"] = At<Address>(header, 0xBC);
    result["sell_tab_index"] = U32(sellTab + 0x3C);
    result["sell_tabs"] = std::move(sellTabs);
    result["buy_first_index"] = U32(At<Address>(header, 0xB0) + 0x38);
    result["sell_first_index"] = U32(At<Address>(header, 0xB8) + 0x38);
    result["buy_selected_index"] = At<std::int32_t>(header, 0xF0);
    result["sell_selected_index"] = At<std::int32_t>(header, 0xF4);
    result["busy"] = At<Address>(header, 0xFC) != 0;

    for (const bool buying : {true, false}) {
        const auto array = At<Address>(header, buying ? (tab == 0 ? 0xC4 : 0xC8) : 0xD4);
        const auto length = array != 0 ? U32(array - 4) : 0;
        if (length > 4096) throw ReadError("ordinary shop item count exceeds the observation bound");
        const auto records = length != 0 ? Bytes(array, length * 0x48) : std::vector<unsigned char>{};
        auto items = Value::Array();
        for (Address index = 0; index < length; ++index) {
            const auto record = index * 0x48;
            const auto id = SecureValue(array + record);
            if (id < 1000000 || id >= 6000000) throw ReadError("ordinary shop item ID is outside the supported range");
            const auto rawPrice = At<std::int32_t>(records, record + 0x18);
            const auto group = id / 10000;
            const bool priceUnknown = (buying && discount != 0 && (group == 200 || group == 201 || group == 202)) ||
                (!buying && (group == 207 || group == 233));
            items.Add(Value::Object({{"index", index}, {"item_id", id},
                {"unit_price", priceUnknown ? Value{} : Value(rawPrice)}, {"raw_price", rawPrice},
                {"quantity", At<std::int32_t>(records, record + (buying ? 0x30 : 0x34))},
                {"bundle_quantity", At<std::int32_t>(records, record + 0x38)},
                {"inventory_type", buying ? Value{} : Value(id / 1000000)},
                {"inventory_slot", buying ? Value{} : Value(At<std::int32_t>(records, record + 0x0C))}}));
        }
        result[buying ? "buy_items" : "sell_items"] = std::move(items);
    }
    if (prompts.size() > 1) unavailable.Add("multiple ordinary shop quantity dialogs are present");
    if (prompts.size() == 1) {
        const auto prompt = prompts.front();
        const auto edit = U32(prompt + 0x66C);
        if (edit == 0 || U32(edit) != 0xAF2BA8) throw ReadError("shop quantity edit control does not match V17");
        const auto pointer = U32(edit + 0x34);
        const auto length = pointer != 0 ? U32(pointer - 4) : 0;
        if (length > 32) throw ReadError("shop quantity text exceeds the observation bound");
        const auto bytes = length != 0 ? Bytes(pointer, length) : std::vector<unsigned char>{};
        const std::string text(bytes.begin(), bytes.end());
        if (std::any_of(bytes.begin(), bytes.end(), [](unsigned char value) { return value > 127; }))
            throw ReadError("shop quantity text is not ASCII");
        const bool decimal = !text.empty() && std::all_of(text.begin(), text.end(),
            [](char value) { return value >= '0' && value <= '9'; });
        const auto parsed = decimal ? Value(std::strtod(text.c_str(), nullptr)) : Value{};
        result["quantity_dialog"] = Value::Object({{"window", Window(prompt)}, {"text", text},
            {"value", parsed}, {"minimum", U32(prompt + 0xD0)}, {"maximum", U32(prompt + 0xD4)}});
    }
    result["not_covered"] = StringArray({"cash_shop", "discounted_potions", "rechargeable_items"});
    result["unavailable"] = std::move(unavailable);
    return result;
}

}
