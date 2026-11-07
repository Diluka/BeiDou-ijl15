// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MapleControl {

// Unknown is kept separate from a known false value in both Lua and host results.
enum class Fact { Unknown, False, True };
enum class ActionKind { Move, Face, Jump, Attack, Pickup, UseItem, Stop };

struct Character {
    std::uint32_t id = 0;
    std::string name;
    // level, job, str, dex, int, luk, hp, max_hp, mp, max_mp, ap, sp, exp
    std::array<std::uint32_t, 13> values{};
    std::array<bool, 13> known{};
};

struct Monster {
    std::uint32_t objectId = 0;
    std::uint32_t templateId = 0;
    bool templateKnown = false;
    std::string name;
    double x = 0;
    double y = 0;
    bool active = false;
};

struct Item {
    int inventoryType = 0;
    int slot = 0;
    std::uint32_t itemId = 0;
    int quantity = 0;
    bool quantityKnown = false;
};

struct Drop {
    std::uint32_t objectId = 0;
    double x = 0;
    double y = 0;
    std::uint32_t itemId = 0;
    bool itemKnown = false;
    std::uint32_t money = 0;
    bool moneyKnown = false;
    Fact landed = Fact::Unknown;
};

struct State {
    std::string instanceId;
    std::uint64_t sequence = 0;
    std::uint64_t observedAtMs = 0; // Unix milliseconds, supplied by the host.
    std::uint64_t sceneGeneration = 0;
    bool inGame = false; // false means the host has positively observed leaving the game.
    bool ready = false;
    bool mapKnown = false;
    std::uint32_t mapId = 0;
    bool characterKnown = false;
    Character character;
    bool positionKnown = false;
    double x = 0;
    double y = 0;
    bool facingKnown = false;
    bool facingLeft = false;
    bool monstersKnown = false;
    std::vector<Monster> monsters;
    bool inventoryKnown = false;
    std::vector<Item> inventory;
    bool dropsKnown = false;
    std::vector<Drop> drops;
    Fact settled = Fact::Unknown;
};

struct Action {
    ActionKind kind = ActionKind::Stop;
    std::string direction; // left/right/up/down; Face accepts left/right.
    std::uint32_t durationMs = 0;
    std::uint32_t itemId = 0;
};

struct ActionResult {
    bool dispatched = false;
    bool controlReleased = false;
    Fact settled = Fact::Unknown;
    std::string effect = "unknown";
    std::string reason;
};

// Each host supplies only native execution and its current state. The Lua
// scheduler, state projection and public semantic API are compiled once per host.
class Adapter {
public:
    virtual ~Adapter() = default;
    virtual const char* Backend() const = 0;
    virtual const State& Observe() const = 0;
    virtual bool Supports(ActionKind kind) const = 0;
    virtual bool Begin(const Action& action, std::string& error) = 0;
    virtual ActionResult Result() const = 0;
    virtual void Release() = 0;
    virtual bool Released() const = 0;
    virtual bool ReleasePending() const = 0;
    virtual bool Logs(const std::string& scriptId, const std::vector<std::string>& lines) = 0;
    virtual bool SupportsRawKeys() const { return false; }
    virtual bool RawKeys(const std::vector<std::string>&, std::uint32_t, std::string& error) {
        error = "unsupported: raw_keys";
        return false;
    }
    virtual bool SupportsSkill() const { return false; }
    virtual bool Skill(std::uint32_t, int&, std::string& error) {
        error = "unsupported: skill";
        return false;
    }
};

struct Status {
    std::string id;
    std::string status;
    std::string reason;
    std::string error;
    std::uint64_t resumes = 0;
    std::uint64_t budgetYields = 0;
    double lastResumeUs = 0;
    double maxResumeUs = 0;
};

class Runtime {
public:
    explicit Runtime(Adapter& adapter);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Start(const std::string& scriptId, const std::string& source,
        std::uint32_t limitMs, std::string& error);
    // Call after the host has updated native state and key-release facts.
    void Tick(bool sceneChanged = false);
    void Cancel(const std::string& reason);
    bool Running() const;
    const Status* CurrentStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
