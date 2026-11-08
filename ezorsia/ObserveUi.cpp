// SPDX-License-Identifier: AGPL-3.0-or-later
#include "stdafx.h"
#include "NativeObservation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <sstream>

namespace NativeObservation {
namespace {
using Address = std::uint32_t;

template<class T> T At(const std::vector<unsigned char>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::string Hex(Address value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct OptionalRect {
    bool known = false;
    Rect value;
};

struct Point {
    bool known = false;
    int x = 0;
    int y = 0;
};

Value Rectangle(const Rect& rect) {
    return Value::Object({{"left", rect.left}, {"top", rect.top},
        {"right", rect.right}, {"bottom", rect.bottom}});
}

Value Rectangle(const OptionalRect& rect) {
    return rect.known ? Rectangle(rect.value) : Value();
}

Rect Translate(const Rect& rect, int x, int y) {
    return {rect.left + x, rect.top + y, rect.right + x, rect.bottom + y};
}

OptionalRect Camera() {
    const auto renderer = Read<Address>(0x00BF14EC);
    if (renderer == 0 || Read<Address>(renderer) != 0x5042D154) return {};
    const auto bytes = Bytes(renderer, 0x108);
    const Rect bounds{At<int>(bytes, 0xF8), At<int>(bytes, 0xFC),
        At<int>(bytes, 0x100), At<int>(bytes, 0x104)};
    const int width = At<int>(bytes, 0x20);
    const int height = At<int>(bytes, 0x24);
    const int centerX = At<int>(bytes, 0xC4);
    const int centerY = At<int>(bytes, 0xC8);
    if (bounds.right - bounds.left != width || bounds.bottom - bounds.top != height ||
        bounds.left != centerX - width / 2 || bounds.top != centerY - height / 2) return {};
    return {true, bounds};
}

struct Button {
    unsigned int id = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    Point local;
    Point absolute;
    std::string source;
    bool uiEligible = false;
};

struct WindowLayout {
    OptionalRect layer;
    OptionalRect vector;
    OptionalRect game;
    std::string positionSource;
    std::vector<Button> buttons;
    std::vector<std::string> problems;
};

WindowLayout ReadWindowLayout(Address address, const std::vector<unsigned char>& header,
    const OptionalRect& camera) {
    WindowLayout layout;
    const auto layer = At<Address>(header, 0x18);
    const int width = At<int>(header, 0x24);
    const int height = At<int>(header, 0x28);
    if (layer != 0 && Read<Address>(layer) == 0x5042E3F0) {
        const auto values = Read<std::array<int, 4>>(layer + 0x54);
        layout.layer = {true, {values[0], values[1], values[2], values[3]}};
        if (values[2] - values[0] == width && values[3] - values[1] == height) {
            layout.vector = layout.layer;
            layout.positionSource = "Gr2D cached layer bounds matched CWnd dimensions";
            if (camera.known) layout.game = {true,
                Translate(layout.layer.value, -camera.value.left, -camera.value.top)};
        }
    }
    const auto children = At<std::array<Address, 3>>(header, 0x58);
    if (children[0] > 4096) throw ReadError("CWnd child count exceeds the supported range");
    Address node = children[1];
    for (Address index = 0; index < children[0]; ++index) {
        if (node == 0) throw ReadError("CWnd child list ended before its recorded count");
        const auto child = Read<Address>(node + 4);
        const auto control = Bytes(child, 0x28);
        if (At<Address>(control, 0) == 0x00AF0C10) {
            Button button;
            button.id = At<unsigned int>(control, 0x14);
            const auto origin = At<Address>(control, 0x18);
            button.width = At<unsigned int>(control, 0x1C);
            button.height = At<unsigned int>(control, 0x20);
            if (At<Address>(control, 0x24) != address)
                throw ReadError("CWnd button parent does not match the observed window");
            if (origin != 0 && Read<Address>(origin) == 0x5140C0FC) {
                const auto vector = Bytes(origin, 0x28);
                const auto state = At<Address>(vector, 0x24);
                if (state != 0) {
                    const auto motion = Bytes(state, 0x78);
                    if (At<Address>(motion, 0) == 0x5140D8F0) {
                        button.local = {true, At<int>(motion, 0x68), At<int>(motion, 0x6C)};
                        button.absolute = {true, At<int>(motion, 0x70), At<int>(motion, 0x74)};
                        button.source = "Shape2D cached local +0x68 and absolute +0x70";
                        if (At<Address>(motion, 0x1C) == layer) {
                            const int parentX = At<int>(motion, 0x60);
                            const int parentY = At<int>(motion, 0x64);
                            layout.vector = {true, {parentX, parentY, parentX + width, parentY + height}};
                            layout.positionSource = "Shape2D button parent snapshot";
                            if (camera.known) layout.game = {true,
                                Translate(layout.vector.value, -camera.value.left, -camera.value.top)};
                            button.uiEligible = true;
                        }
                    }
                }
                else {
                    button.local = {true, At<int>(vector, 0x1C), At<int>(vector, 0x20)};
                    button.absolute = button.local;
                    button.source = "Shape2D fixed vector without parent origin";
                }
            }
            layout.buttons.push_back(button);
        }
        const auto next = Read<Address>(node - 12);
        node = next == 0 ? 0 : next + 16;
    }
    if (Read<std::array<Address, 3>>(address + 0x58) != children) {
        layout.buttons.clear();
        layout.problems.push_back("CWnd child list changed while collecting the observation");
    }
    if (!camera.known)
        layout.problems.push_back("Scene camera origin is unavailable; game/root coordinates cannot be used for clicking");
    return layout;
}

Value DialogButtons(const WindowLayout& layout, const OptionalRect& camera,
    const std::vector<unsigned int>& verified = {}) {
    auto output = Value::Array();
    for (const auto& button : layout.buttons) {
        const bool projected = button.absolute.known && camera.known;
        const bool observed = std::find(verified.begin(), verified.end(), button.id) != verified.end();
        output.Add(Value::Object({
            {"control_id", button.id}, {"width", button.width}, {"height", button.height},
            {"layout_x", button.local.known ? Value(button.local.x) : Value()},
            {"layout_y", button.local.known ? Value(button.local.y) : Value()},
            {"game_x", projected ? Value(button.absolute.x - camera.value.left) : Value()},
            {"game_y", projected ? Value(button.absolute.y - camera.value.top) : Value()},
            {"uncalibrated_vector_x", button.absolute.known ? Value(button.absolute.x) : Value()},
            {"uncalibrated_vector_y", button.absolute.known ? Value(button.absolute.y) : Value()},
            {"position_source", button.source.empty() ? Value() : Value(button.source)},
            {"root_rect", Value()},
            {"click_validation", observed ? "verified-observed-case" : "not-covered"}
        }));
    }
    return output;
}

void ApplyLayout(Value& output, const WindowLayout& layout, const OptionalRect& camera,
    const std::vector<unsigned int>& verified = {}) {
    output["layer_rect"] = Rectangle(layout.layer);
    output["uncalibrated_vector_rect"] = Rectangle(layout.vector);
    output["game_rect"] = Rectangle(layout.game);
    output["root_rect"] = Value();
    output["window_position_source"] = layout.positionSource.empty() ? Value() : Value(layout.positionSource);
    output["buttons"] = DialogButtons(layout, camera, verified);
}

Value Tables(const std::array<Address, 3>& tables) {
    return Value::Array({Hex(tables[0]), Hex(tables[1]), Hex(tables[2])});
}

const char* WindowName(Address table) {
    switch (table) {
    case 0x00B3F13C: return "CWndMan";
    case 0x00B3E100: return "CUITalk";
    case 0x00B39108: return "CUIItem";
    case 0x00B3B908: return "CUIStat";
    case 0x00B3B5CC: return "CUISkill";
    default: return nullptr;
    }
}

const char* KeyboardName(Address table) {
    switch (table) {
    case 0x00B3F0F0: return "CWndMan::IUIMsgHandler";
    case 0x00B3E0B4: return "CUITalk::IUIMsgHandler";
    case 0x00AF0BC4: return "CCtrlButton::IUIMsgHandler";
    default: return nullptr;
    }
}

struct Choice {
    int id = 0;
    std::string text;
    Value indices = Value::Array();
    Rect bounds;
};

struct DialogCapture {
    Value value;
    OptionalRect camera;
};

DialogCapture CaptureDialog() {
    DialogCapture capture;
    auto& result = capture.value;
    result = Value::Object({
        {"source", "client-native-api"}, {"readonly", true},
        {"dialog_open", Value()}, {"script_dialog_open", Value()},
        {"focused_talk_dialog_open", Value()}, {"registered_talk_dialog_open", Value()},
        {"registered_talk_count", Value()}, {"dialog_reference", Value()}, {"dialog_address", Value()},
        {"focused_window", Value()}, {"keyboard_handler", Value()}, {"npc_id", Value()},
        {"dialog_type", Value()}, {"layout_validation", "native-task-text-verified"},
        {"button_layout_validation", "camera-translation-unavailable"},
        {"camera_viewport", Value()}, {"camera_source", Value()}, {"layer_rect", Value()},
        {"game_rect", Value()}, {"uncalibrated_vector_rect", Value()}, {"root_rect", Value()},
        {"game_geometry", Value()}, {"window_position_source", Value()},
        {"text_tokens", Value::Array()}, {"lines", Value::Array()}, {"choices", Value::Array()},
        {"buttons", Value::Array()}, {"unavailable", Value::Array()},
        {"not_covered", Value::Array({"automatic_button_label_recognition",
            "unobserved_type_control_combinations", "unobserved_dialog_types", "links_outside_type4",
            "unfocused_unregistered_native_dialogs", "unknown_CWnd_RTTI_and_semantics"})}
    });
    auto unavailable = Value::Array();
    capture.camera = Camera();
    if (capture.camera.known) {
        result["camera_viewport"] = Rectangle(capture.camera);
        result["camera_source"] = "g_gr [0xBF14EC] renderer world viewport +0xF8";
        result["button_layout_validation"] = "camera-projection-verified-click-pending";
    }

    Address dialog = 0;
    Address focusedWindow = 0;
    Address scriptDialog = 0;
    bool focusKnown = false;
    bool scriptKnown = false;
    bool haveFocusedLayout = false;
    WindowLayout focusedLayout;
    const auto windowManager = Read<Address>(0x00BEC20C);
    if (windowManager != 0 && Read<Address>(windowManager) == 0x00B3F13C) {
        const auto focus = Read<std::array<Address, 3>>(windowManager + 0x80);
        focusedWindow = focus[0];
        const auto keyboardHandler = focus[2];
        focusKnown = true;
        bool talk = false;
        if (focusedWindow != 0) {
            const auto window = Bytes(focusedWindow, 0x64);
            const auto tables = At<std::array<Address, 3>>(window, 0);
            // The focus observer has only these two named CWnd layouts verified.
            const char* name = tables[0] == 0x00B3F13C ? "CWndMan" :
                tables[0] == 0x00B3E100 ? "CUITalk" : nullptr;
            auto focused = Value::Object({
                {"address", Hex(focusedWindow)}, {"vtables", Tables(tables)},
                {"type_name", name == nullptr ? Value() : Value(name)},
                {"type_name_source", name == nullptr ? Value() : Value("verified-vtable")},
                {"rtti_name", Value()}, {"width", At<int>(window, 0x24)}, {"height", At<int>(window, 0x28)}
            });
            focusedLayout = ReadWindowLayout(focusedWindow, window, capture.camera);
            haveFocusedLayout = true;
            ApplyLayout(focused, focusedLayout, capture.camera);
            result["focused_window"] = focused;
            for (const auto& problem : focusedLayout.problems) unavailable.Add(problem);
            talk = tables == std::array<Address, 3>{{0x00B3E100, 0x00B3E0B4, 0x00B3E0B0}};
        }
        result["focused_talk_dialog_open"] = talk;
        if (keyboardHandler != 0) {
            const auto table = Read<Address>(keyboardHandler);
            const char* name = KeyboardName(table);
            result["keyboard_handler"] = Value::Object({
                {"address", Hex(keyboardHandler)}, {"vtables", Value::Array({Hex(table)})},
                {"type_name", name == nullptr ? Value() : Value(name)},
                {"type_name_source", name == nullptr ? Value() : Value("verified-vtable")},
                {"rtti_name", Value()}
            });
        }
        if (talk) {
            dialog = focusedWindow;
            result["dialog_reference"] = "CWndMan focused CWnd +0x80";
        }
    }
    else unavailable.Add("Current CWndMan window reference is unavailable");

    const auto manager = Read<Address>(0x00BED770);
    if (manager == 0) unavailable.Add("No current CScriptMan object");
    else if (Read<Address>(manager) != 0x00AFDED8)
        unavailable.Add("CScriptMan vtable does not match the verified layout");
    else {
        scriptDialog = Read<Address>(manager + 0x0C);
        scriptKnown = true;
        result["script_dialog_open"] = scriptDialog != 0;
        if (scriptDialog != 0 && dialog == 0) {
            dialog = scriptDialog;
            result["dialog_reference"] = "CScriptMan+0x0C";
        }
    }

    const auto registry = Read<std::array<Address, 5>>(0x00BF1548);
    if (registry[0] != 0x00B3DF98) unavailable.Add("CUITalk active-list vtable does not match the verified layout");
    else {
        const auto count = registry[2];
        const auto head = registry[3];
        const auto tail = registry[4];
        if (count > 4096 || (count != 0) != (head != 0) || (count != 0) != (tail != 0))
            throw ReadError("CUITalk active-list bounds do not match the verified layout");
        result["registered_talk_count"] = count;
        result["registered_talk_dialog_open"] = count != 0;
        if (dialog == 0 && tail != 0) {
            dialog = Read<Address>(tail);
            result["dialog_reference"] = "CUITalk active list 0xBF1548 tail";
        }
    }

    if (dialog != 0) {
        if (Read<Address>(dialog) == 0x00B3E100) {
            const auto header = Bytes(dialog, 0x6E8);
            const auto dialogType = At<Address>(header, 0x7C);
            result["dialog_open"] = true;
            result["dialog_address"] = Hex(dialog);
            result["npc_id"] = At<Address>(header, 0xA0);
            result["dialog_type"] = dialogType;
            const bool scriptManaged = scriptKnown && scriptDialog == dialog;
            std::vector<unsigned int> verified;
            if (dialogType == 0) verified = scriptManaged ? std::vector<unsigned int>{8193} :
                std::vector<unsigned int>{1, 2, 8193};
            else if (dialogType == 1) verified = {6};
            if (capture.camera.known && (!verified.empty() || dialogType == 4))
                result["button_layout_validation"] = "observed-cases-verified";
            const auto layout = focusedWindow == dialog && haveFocusedLayout ? focusedLayout :
                ReadWindowLayout(dialog, header, capture.camera);
            ApplyLayout(result, layout, capture.camera, verified);
            if (focusedWindow != dialog)
                for (const auto& problem : layout.problems) unavailable.Add(problem);

            const auto array = At<Address>(header, 0x6C4);
            if (array != 0) {
                const auto count = Read<Address>(array - 4);
                if (count > 4096) throw ReadError("CUITalk text count exceeds the supported range");
                const auto textLayout = Bytes(array, count * 0x44);
                const int contentX = At<int>(header, 0x80);
                const int contentY = At<int>(header, 0x84);
                const int scrollY = At<int>(header, 0x90);
                auto tokens = Value::Array();
                std::vector<std::pair<int, std::string>> rows;
                std::vector<Choice> choices;
                std::map<int, std::size_t> choiceIndices;
                for (Address index = 0; index < count; ++index) {
                    const auto offset = static_cast<std::size_t>(index) * 0x44;
                    const auto kind = At<Address>(textLayout, offset);
                    if (kind != 0 || At<Address>(textLayout, offset + 0x10) == 0) continue;
                    const int row = At<int>(textLayout, offset + 8);
                    const auto text = ZxString(array + offset + 0x10);
                    const int x = At<int>(textLayout, offset + 0x18);
                    const int y = At<int>(textLayout, offset + 0x1C);
                    const int width = At<int>(textLayout, offset + 0x20);
                    const int height = At<int>(textLayout, offset + 0x24);
                    const int dialogX = x + contentX;
                    const int dialogY = y + contentY - scrollY;
                    const int linkId = At<int>(textLayout, offset + 0x28);
                    tokens.Add(Value::Object({
                        {"index", index}, {"row", row}, {"text", text}, {"layout_x", x}, {"layout_y", y},
                        {"dialog_x", dialogX}, {"dialog_y", dialogY},
                        {"game_x", layout.game.known ? Value(layout.game.value.left + dialogX) : Value()},
                        {"game_y", layout.game.known ? Value(layout.game.value.top + dialogY) : Value()},
                        {"width", width}, {"height", height}, {"link_id", linkId >= 0 ? Value(linkId) : Value()}
                    }));
                    const auto foundRow = std::find_if(rows.begin(), rows.end(),
                        [row](const std::pair<int, std::string>& entry) { return entry.first == row; });
                    if (foundRow == rows.end()) rows.emplace_back(row, text);
                    else foundRow->second += text;
                    if (linkId >= 0) {
                        const auto found = choiceIndices.find(linkId);
                        std::size_t choiceIndex = 0;
                        if (found == choiceIndices.end()) {
                            choiceIndex = choices.size();
                            choiceIndices.emplace(linkId, choiceIndex);
                            Choice choice;
                            choice.id = linkId;
                            choice.bounds = {dialogX, dialogY, dialogX + width, dialogY + height};
                            choices.push_back(choice);
                        }
                        else choiceIndex = found->second;
                        auto& choice = choices[choiceIndex];
                        choice.text += text;
                        choice.indices.Add(index);
                        choice.bounds.left = (std::min)(choice.bounds.left, dialogX);
                        choice.bounds.top = (std::min)(choice.bounds.top, dialogY);
                        choice.bounds.right = (std::max)(choice.bounds.right, dialogX + width);
                        choice.bounds.bottom = (std::max)(choice.bounds.bottom, dialogY + height);
                    }
                }
                auto lines = Value::Array();
                for (const auto& row : rows) lines.Add(row.second);
                auto exportedChoices = Value::Array();
                for (const auto& choice : choices) {
                    exportedChoices.Add(Value::Object({
                        {"link_id", choice.id}, {"text", choice.text}, {"token_indices", choice.indices},
                        {"dialog_rect", Rectangle(choice.bounds)},
                        {"game_rect", layout.game.known ? Rectangle(Translate(choice.bounds,
                            layout.game.value.left, layout.game.value.top)) : Value()},
                        {"root_rect", Value()},
                        {"click_validation", dialogType == 4 ? "verified-observed-case" : "not-covered"}
                    }));
                }
                result["text_tokens"] = tokens;
                result["lines"] = lines;
                result["choices"] = exportedChoices;
                if (Read<Address>(dialog + 0x6C4) != array) {
                    result["text_tokens"] = Value::Array();
                    result["lines"] = Value::Array();
                    result["choices"] = Value::Array();
                    unavailable.Add("CUITalk text array changed while collecting the observation");
                }
            }
        }
        else unavailable.Add("Active window is not a verified CUITalk; it cannot be treated as closed");
    }

    const bool scriptChanged = scriptKnown && Read<Address>(manager + 0x0C) != scriptDialog;
    const bool registryChanged = Read<std::array<Address, 5>>(0x00BF1548) != registry;
    const bool focusChanged = focusKnown && Read<Address>(windowManager + 0x80) != focusedWindow;
    if (scriptChanged || registryChanged || focusChanged) {
        for (const char* key : {"dialog_open", "script_dialog_open", "focused_talk_dialog_open",
            "registered_talk_dialog_open", "registered_talk_count", "dialog_reference", "dialog_address",
            "focused_window", "keyboard_handler", "npc_id", "dialog_type", "layer_rect", "game_rect",
            "uncalibrated_vector_rect"}) result[key] = Value();
        for (const char* key : {"text_tokens", "lines", "choices", "buttons"}) result[key] = Value::Array();
        unavailable.Add("Active dialog references changed while collecting the observation");
    }
    result["unavailable"] = unavailable;
    return capture;
}

Value CaptureWindow(Address address, const OptionalRect& camera) {
    const auto header = Bytes(address, 0x64);
    const auto tables = At<std::array<Address, 3>>(header, 0);
    const auto width = At<int>(header, 0x24);
    const auto height = At<int>(header, 0x28);
    const auto layout = ReadWindowLayout(address, header, camera);
    auto buttons = Value::Array();
    for (const auto& button : layout.buttons) if (button.uiEligible) {
        buttons.Add(Value::Object({{"control_id", button.id},
            {"layout_x", button.local.x}, {"layout_y", button.local.y},
            {"width", button.width}, {"height", button.height}}));
    }
    Value inventory;
    if (tables[0] == 0x00B39108) {
        const auto fields = Read<std::array<Address, 2>>(address + 0x5E0);
        inventory = Value::Object({{"first_slot", fields[0]}, {"selected_type", fields[1]},
            {"expanded", Read<Address>(address + 0x604) != 0}});
    }
    Value skills;
    if (tables[0] == 0x00B3B5CC) {
        const auto tab = Read<Address>(address + 0x5B8);
        const auto scroll = Read<Address>(address + 0x5C0);
        if (Read<Address>(scroll) != 0x00AF2E94) throw ReadError("Skill scrollbar type does not match the verified layout");
        const auto array = Read<Address>(address + 0x5F4);
        const auto length = array == 0 ? 0 : Read<Address>(array - 4);
        if (length > 512) throw ReadError("Visible skill list exceeds the supported range");
        auto ids = Value::Array();
        for (Address skillIndex = 0; skillIndex < length; ++skillIndex) {
            const auto skill = Read<Address>(array + skillIndex * 8 + 4);
            ids.Add(Read<Address>(skill));
        }
        skills = Value::Object({{"ids", ids}, {"first_index", Read<Address>(scroll + 0x38)},
            {"selected_tab", Read<Address>(tab + 0x3C)}, {"tab_count", Read<Address>(tab + 0x48)}});
    }
    const auto name = WindowName(tables[0]);
    return Value::Object({{"address", Hex(address)}, {"vtables", Tables(tables)},
        {"type_name", name == nullptr ? Value() : Value(name)}, {"width", width}, {"height", height},
        {"game_rect", Rectangle(layout.game)}, {"root_rect", Value()},
        {"buttons", buttons}, {"skills", skills}, {"inventory", inventory}});
}
}

Value Dialog() { return CaptureDialog().value; }

Value Window(std::uintptr_t address) { return CaptureWindow(static_cast<Address>(address), Camera()); }

Value Ui() {
    auto dialog = CaptureDialog();
    auto result = Value::Object({
        {"source", "client-native-api"}, {"readonly", true}, {"windows", Value::Array()},
        {"dialog", dialog.value}, {"game_geometry", Value()}, {"unavailable", Value::Array()}
    });
    if (!dialog.camera.known) {
        result["unavailable"] = Value::Array({"Scene camera is unavailable; window coordinates are not projected"});
        return result;
    }
    const auto before = Read<std::array<Address, 3>>(0x00BF1650);
    if (before[0] > 256) throw ReadError("Active CWnd count exceeds the supported range");
    Address node = before[1];
    auto windows = Value::Array();
    for (Address index = 0; index < before[0]; ++index) {
        if (node == 0) throw ReadError("Active CWnd list ended before its recorded count");
        const auto address = Read<Address>(node);
        windows.Add(CaptureWindow(address, dialog.camera));
        const auto next = Read<Address>(node - 12);
        node = next == 0 ? 0 : next + 16;
    }
    if (Read<std::array<Address, 3>>(0x00BF1650) != before)
        throw ReadError("Active CWnd list changed while collecting the observation");
    result["windows"] = windows;
    return result;
}
}
