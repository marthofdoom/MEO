// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M3a: THE PLAYABLE SOCKET CORE. The M2 equip-anything scaffold is gone;
// socketing is now deliberate and catalog-driven:
//   - MEO.esp ships the gem MISC items (14 weapon gems x 5 levels) and the
//     Gem Pouch lesser power. The DLL grants the power natively (no Papyrus
//     anywhere; the ESP's script-era VMAD references stay unshipped/harmless).
//   - Cast Gem Pouch with a weapon drawn -> the first gem in inventory is
//     socketed into it: "Fire I Glass Dagger" + a real created enchantment at
//     the catalog magnitude for that gem/level. The gem item is consumed.
//   - Catalog is EMBEDDED at build time (native/GemCatalog.h, generated from
//     data/gem_catalog.json) — no runtime JSON, nothing to mis-path.
//
// ── SAVE-SAFETY RULES (from v0.7.0 every update must load older saves) ──
//   1. Co-save 'GEMS' schema is VERSIONED. Readers for every shipped version
//      stay forever; writers write only the newest. Never reorder/remove
//      fields — extend via a version bump + migration in LoadCallback.
//   2. Socket identity in the co-save is the gem's STABLE STRING gid (catalog
//      key), never an array index — indexes change, gids don't.
//   3. Record key is (baseFormID, uniqueID): engine uniqueIDs are only unique
//      PER BASE FORM (a favorited dagger and a favorited sword can share
//      uid 108) — uid alone would collide.
//   4. MEO.esp FormIDs are frozen (generator constants). Forms are only ever
//      ADDED, never renumbered or deleted.
//   5. Created enchantments (FF forms) are engine-persisted in the save and
//      self-contained — a DLL update never invalidates an already-socketed
//      item; the co-save record is what levels/rebuilds it.
//
// The proven M2 stamp recipe (all engine flows, per the standing rule):
//   ExtraUniqueID -> ExtraTextDisplayData (forced SetName + engine
//   display-name builder reconcile; NO BRACKETS — Lorerim's UI strips leading
//   [...] tags from the activate rollover) -> created enchantment via
//   BGSCreatedObjectManager::AddWeaponEnchantment -> ExtraEnchantment ->
//   Actor::UpdateWeaponAbility when worn.

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "GemCatalog.h"

namespace {

// ── Co-save state ─────────────────────────────────────────────────────
struct SocketRecord {
    std::string  gid;    // stable catalog identity
    std::uint8_t level;  // 1..5
    float        xp;
};
// key = (baseFormID << 16) | uniqueID  — see save-safety rule 3.
using InstKey = std::uint64_t;
constexpr InstKey MakeKey(RE::FormID a_base, std::uint16_t a_uid) {
    return (static_cast<InstKey>(a_base) << 16) | a_uid;
}
std::unordered_map<InstKey, SocketRecord> g_sockets;
std::uint16_t g_nextUID = 0x9000;  // our range, clear of engine-assigned ids
bool          g_starterGranted = false;

constexpr std::uint32_t kSerID = 'MEO1';
constexpr std::uint32_t kRecGems = 'GEMS';
constexpr std::uint32_t kSerVersion = 3;  // v3: (base,uid) key + gid string + starter flag

// ── Catalog resolved against the live load order (kDataLoaded) ───────
constexpr const char* kPluginName = "MEO.esp";
constexpr RE::FormID  kPouchSpellID = 0x803;  // MEO.esp-local

struct ResolvedGem {
    const meo::GemDef*                  def = nullptr;
    RE::EffectSetting*                  mgef = nullptr;   // null = disabled (missing master)
    std::array<RE::TESObjectMISC*, 5>   items{};
};
std::vector<ResolvedGem>                          g_gems;
std::unordered_map<RE::FormID, std::pair<int, int>> g_gemByItem;  // item -> {gemIdx, level}
std::unordered_map<std::string_view, int>         g_gemByGid;
RE::SpellItem*                                    g_pouchSpell = nullptr;

void SetupLog() {
    auto logDir = SKSE::log::log_directory();
    if (!logDir) {
        SKSE::stl::report_and_fail("MEO: unable to resolve the SKSE log directory");
    }
    auto logPath = *logDir / "MEO.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

void ResolveCatalog() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        spdlog::error("TESDataHandler missing — catalog not resolved");
        return;
    }
    g_pouchSpell = dh->LookupForm<RE::SpellItem>(kPouchSpellID, kPluginName);
    if (!g_pouchSpell) {
        spdlog::error("Gem Pouch power not found in {} — is the ESP enabled?", kPluginName);
    }
    int ok = 0;
    for (const auto& def : meo::kWeaponGems) {
        ResolvedGem rg;
        rg.def = &def;
        rg.mgef = dh->LookupForm<RE::EffectSetting>(def.mgefID, def.plugin);
        if (!rg.mgef) {
            spdlog::warn("gem '{}' disabled: MGEF {:06X} not found in {}", def.gid, def.mgefID, def.plugin);
        }
        const int levels = def.singleLevel ? 1 : 5;
        for (int lv = 0; lv < levels; ++lv) {
            rg.items[lv] = dh->LookupForm<RE::TESObjectMISC>(def.gemItem[lv], kPluginName);
            if (rg.items[lv] && rg.mgef) {
                g_gemByItem[rg.items[lv]->GetFormID()] = { static_cast<int>(g_gems.size()), lv + 1 };
            }
        }
        g_gemByGid[def.gid] = static_cast<int>(g_gems.size());
        if (rg.mgef) {
            ++ok;
        }
        g_gems.push_back(rg);
    }
    spdlog::info("catalog resolved: {}/{} weapon gems live, {} socketable gem items, pouch={}",
                 ok, std::size(meo::kWeaponGems), g_gemByItem.size(),
                 g_pouchSpell ? "ok" : "MISSING");
}

// ── The stamp: paint one socket onto one instance (engine flows only) ─
// Returns the uid used, or 0 on failure. Caller handles UpdateWeaponAbility
// (worn items) and gem-item consumption. a_xp carries accumulated XP through
// a level-up re-stamp (fresh sockets pass 0).
std::uint16_t StampInstance(RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                            int a_gemIdx, int a_level, float a_xp = 0.0f) {
    const auto& rg = g_gems[a_gemIdx];
    if (!rg.mgef || !a_base || !a_xList) {
        return 0;
    }
    const int lvIdx = std::clamp(a_level, 1, 5) - 1;

    std::uint16_t uid = 0;
    if (auto* xid = a_xList->GetByType<RE::ExtraUniqueID>()) {
        uid = xid->uniqueID;  // engine-assigned is fine: key includes baseID
    } else {
        uid = g_nextUID++;
        a_xList->Add(new RE::ExtraUniqueID(a_base->GetFormID(), uid));
    }

    // Forced name + engine reconcile (M2d/M2h). No brackets (M2i).
    const char*       baseName = a_base->GetName();
    const std::string newName = std::format("{} {} {}", rg.def->name, meo::kRoman[lvIdx],
                                            (baseName && *baseName) ? baseName : "Item");
    auto* xText = a_xList->GetByType<RE::ExtraTextDisplayData>();
    if (xText) {
        xText->displayNameText = nullptr;
        xText->ownerQuest = nullptr;
        xText->SetName(newName.c_str());
    } else {
        xText = new RE::ExtraTextDisplayData(newName.c_str());
        a_xList->Add(xText);
    }
    float health = 1.0f;
    if (auto* xHealth = a_xList->GetByType<RE::ExtraHealth>()) {
        health = xHealth->health;
    }
    xText->GetDisplayName(a_base, health);  // engine builder reconciles the record

    // Created enchantment at the catalog magnitude (M2c recipe).
    RE::BSTArray<RE::Effect> effects;
    effects.resize(1);
    auto& eff = effects[0];
    eff.effectItem.magnitude = rg.def->magnitude[lvIdx];
    eff.effectItem.area = 0;
    eff.effectItem.duration = static_cast<std::uint32_t>(rg.def->duration);
    eff.baseEffect = rg.mgef;
    eff.cost = 0.0f;
    auto* ench = RE::BGSCreatedObjectManager::GetSingleton()->AddWeaponEnchantment(effects);
    if (!ench) {
        spdlog::error("AddWeaponEnchantment returned null for '{}'", rg.def->gid);
        return 0;
    }
    // Replace any previous instance enchantment (level-up path re-stamps).
    if (auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>()) {
        xEnch->enchantment = ench;
        xEnch->charge = 500;
        xEnch->removeOnUnequip = false;
    } else {
        a_xList->Add(new RE::ExtraEnchantment(ench, 500, false));
    }

    g_sockets[MakeKey(a_base->GetFormID(), uid)] =
        SocketRecord{ rg.def->gid, static_cast<std::uint8_t>(lvIdx + 1), a_xp };
    spdlog::info("STAMP {:08X}/{}: '{}' gem={} L{} mag={} ench={:08X}; index {} record(s)",
                 a_base->GetFormID(), uid, newName, rg.def->gid, lvIdx + 1,
                 rg.def->magnitude[lvIdx], ench->GetFormID(), g_sockets.size());
    return uid;
}

// ── Pouch cast -> socket the first inventory gem into the drawn weapon ─
void Notify(const std::string& a_msg) {
    RE::DebugNotification(a_msg.c_str());
}

void SocketFromPouch() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }

    // Worn weapon instance (right hand preferred).
    RE::InventoryEntryData* wornEntry = nullptr;
    RE::ExtraDataList*      wornList = nullptr;
    bool                    leftHand = false;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->object->Is(RE::FormType::Weapon) || !entry->extraLists) {
            continue;
        }
        for (auto* xList : *entry->extraLists) {
            if (!xList) {
                continue;
            }
            const bool right = xList->HasType(RE::ExtraDataType::kWorn);
            const bool left = xList->HasType(RE::ExtraDataType::kWornLeft);
            if ((right || left) && (!wornEntry || (right && leftHand))) {
                wornEntry = entry;
                wornList = xList;
                leftHand = !right;
            }
        }
    }
    if (!wornEntry) {
        Notify("Draw a weapon to socket a gem.");
        return;
    }
    if (wornList->HasType(RE::ExtraDataType::kEnchantment)) {
        Notify("That weapon already holds a gem.");
        return;
    }
    if (auto* weap = wornEntry->object->As<RE::TESObjectWEAP>(); weap && weap->formEnchanting) {
        Notify("Enchanted weapons cannot hold gems.");
        return;
    }

    // First gem in inventory, deterministic order (catalog index, then level).
    RE::TESObjectMISC* gemItem = nullptr;
    int gemIdx = -1, gemLevel = 0;
    auto inv = player->GetInventory([](RE::TESBoundObject& obj) { return obj.Is(RE::FormType::Misc); });
    for (const auto& [obj, data] : inv) {
        if (data.first <= 0) {
            continue;
        }
        auto it = g_gemByItem.find(obj->GetFormID());
        if (it == g_gemByItem.end()) {
            continue;
        }
        const auto [idx, lv] = it->second;
        if (!gemItem || idx < gemIdx || (idx == gemIdx && lv < gemLevel)) {
            gemItem = obj->As<RE::TESObjectMISC>();
            gemIdx = idx;
            gemLevel = lv;
        }
    }
    if (!gemItem) {
        Notify("You have no gems to socket.");
        return;
    }

    const auto uid = StampInstance(wornEntry->object, wornList, gemIdx, gemLevel);
    if (!uid) {
        return;
    }
    player->UpdateWeaponAbility(wornEntry->object, wornList, leftHand);
    player->RemoveItem(gemItem, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    const auto& rg = g_gems[gemIdx];
    Notify(std::format("{} {} socketed into {}.", rg.def->name, meo::kRoman[gemLevel - 1],
                       wornEntry->object->GetName()));
}

// ── M3b: kill XP -> level-ups -> re-stamp; mastered gems birth a copy ─
// DESIGN §3: 1 AP per player kill to every socketed gem on worn weapons;
// cumulative thresholds 400/1200/3600/10000 x the gem's xpMult. Level V =
// mastered: births one level-I copy of itself and stops accruing.
float g_xpPerKill = 1.0f;  // [Dev] fXPPerKill in SKSE/Plugins/MEO.ini overrides

void ReadConfig() {
    std::ifstream ini("Data/SKSE/Plugins/MEO.ini");
    std::string   line;
    while (std::getline(ini, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        if (trim(line.substr(0, eq)) == "fXPPerKill") {
            g_xpPerKill = std::strtof(trim(line.substr(eq + 1)).c_str(), nullptr);
        }
    }
    if (g_xpPerKill != 1.0f) {
        spdlog::warn("DEV: fXPPerKill={} (MEO.ini override)", g_xpPerKill);
    }
}

void AwardKillXP(float a_ap) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->object->Is(RE::FormType::Weapon) || !entry->extraLists) {
            continue;
        }
        for (auto* xList : *entry->extraLists) {
            if (!xList) {
                continue;
            }
            const bool right = xList->HasType(RE::ExtraDataType::kWorn);
            const bool left = xList->HasType(RE::ExtraDataType::kWornLeft);
            if (!right && !left) {
                continue;
            }
            auto* xid = xList->GetByType<RE::ExtraUniqueID>();
            if (!xid) {
                continue;
            }
            const auto key = MakeKey(entry->object->GetFormID(), xid->uniqueID);
            auto it = g_sockets.find(key);
            if (it == g_sockets.end()) {
                continue;
            }
            auto& rec = it->second;
            auto gemIt = g_gemByGid.find(rec.gid);
            if (gemIt == g_gemByGid.end() || rec.level >= 5) {
                continue;
            }
            const auto& rg = g_gems[gemIt->second];
            if (!rg.mgef || rg.def->xpMult <= 0.0f) {
                continue;  // single-level / disabled gems never level
            }
            rec.xp += a_ap;
            const float need = meo::kXPThresholds[rec.level - 1] * rg.def->xpMult;
            spdlog::info("[xp] {:08X}/{} {} L{}: {:.0f}/{:.0f}",
                         entry->object->GetFormID(), xid->uniqueID, rec.gid, rec.level, rec.xp, need);
            if (rec.xp < need) {
                continue;
            }
            const int   newLevel = rec.level + 1;
            const float carriedXP = rec.xp;
            StampInstance(entry->object, xList, gemIt->second, newLevel, carriedXP);
            player->UpdateWeaponAbility(entry->object, xList, left);
            Notify(std::format("Your {} gem has grown to {}.", rg.def->name, meo::kRoman[newLevel - 1]));
            if (newLevel == 5 && rg.items[0]) {
                player->AddObjectToContainer(rg.items[0], nullptr, 1, nullptr);
                Notify(std::format("Your mastered {} gem births a new gem.", rg.def->name));
                spdlog::info("[birth] mastered '{}' birthed a level-I copy", rec.gid);
            }
        }
    }
}

class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent> {
public:
    static DeathSink* GetSingleton() {
        static DeathSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
                                          RE::BSTEventSource<RE::TESDeathEvent>*) override {
        if (a_event && a_event->dead && a_event->actorDying && !a_event->actorDying->IsPlayerRef() &&
            a_event->actorKiller && a_event->actorKiller->IsPlayerRef()) {
            SKSE::GetTaskInterface()->AddTask([]() { AwardKillXP(g_xpPerKill); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent> {
public:
    static SpellCastSink* GetSingleton() {
        static SpellCastSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
                                          RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
        if (a_event && g_pouchSpell && a_event->spell == g_pouchSpell->GetFormID() &&
            a_event->object && a_event->object->IsPlayerRef()) {
            SKSE::GetTaskInterface()->AddTask([]() { SocketFromPouch(); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── Crosshair sink (kept from M2g: read-only ground-ref verification) ─
class CrosshairSink : public RE::BSTEventSink<SKSE::CrosshairRefEvent> {
public:
    static CrosshairSink* GetSingleton() {
        static CrosshairSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const SKSE::CrosshairRefEvent* a_event,
                                          RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override {
        if (!a_event || !a_event->crosshairRef) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto& ref = a_event->crosshairRef;
        auto* base = ref->GetBaseObject();
        // Only log instances that carry our data — quiet during normal play.
        if (!base || !base->Is(RE::FormType::Weapon) ||
            !ref->extraList.HasType(RE::ExtraDataType::kUniqueID)) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const char* dfn = ref->GetDisplayFullName();
        spdlog::info("[xhair] ref {:08X} base {:08X} name='{}'",
                     ref->GetFormID(), base->GetFormID(), dfn ? dfn : "null");
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── Player setup: grant the pouch power + one-time starter gems ───────
void EnsurePlayerSetup() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    if (g_pouchSpell && !player->HasSpell(g_pouchSpell)) {
        player->AddSpell(g_pouchSpell);
        spdlog::info("granted Gem Pouch power");
        Notify("You have learned to socket gems (Gem Pouch power).");
    }
    if (!g_starterGranted) {
        int given = 0;
        for (const char* gid : { "firedamage", "frost", "shockdamage" }) {
            auto it = g_gemByGid.find(gid);
            if (it == g_gemByGid.end()) {
                continue;
            }
            const auto& rg = g_gems[it->second];
            if (rg.mgef && rg.items[0]) {
                player->AddObjectToContainer(rg.items[0], nullptr, 1, nullptr);
                ++given;
            }
        }
        // Only consume the one-time flag if something was actually granted —
        // with the ESP missing/disabled the grant must retry next load, not
        // burn itself (v0.7.0 did, poisoning that session's saves).
        if (given > 0) {
            g_starterGranted = true;  // persisted in the co-save on next save
            spdlog::info("starter kit: {} level-I gems granted", given);
            Notify("Starter gems added to your inventory.");
        }
    }
}

// ── SKSE co-save (schema v3 — see save-safety rules in the header) ────
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    if (!a_intfc->OpenRecord(kRecGems, kSerVersion)) {
        spdlog::error("SaveCallback: OpenRecord('GEMS') failed");
        return;
    }
    a_intfc->WriteRecordData(g_nextUID);
    const std::uint8_t starter = g_starterGranted ? 1 : 0;
    a_intfc->WriteRecordData(starter);
    const std::uint32_t count = static_cast<std::uint32_t>(g_sockets.size());
    a_intfc->WriteRecordData(count);
    for (const auto& [key, rec] : g_sockets) {
        const std::uint32_t baseID = static_cast<std::uint32_t>(key >> 16);
        const std::uint16_t uid = static_cast<std::uint16_t>(key & 0xFFFF);
        a_intfc->WriteRecordData(baseID);
        a_intfc->WriteRecordData(uid);
        a_intfc->WriteRecordData(rec.level);
        a_intfc->WriteRecordData(rec.xp);
        const std::uint16_t len = static_cast<std::uint16_t>(rec.gid.size());
        a_intfc->WriteRecordData(len);
        a_intfc->WriteRecordData(rec.gid.data(), len);
    }
    spdlog::info("[save] {} socket record(s), nextUID=0x{:X}, starter={}",
                 g_sockets.size(), g_nextUID, g_starterGranted);
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_sockets.clear();
    g_nextUID = 0x9000;
    g_starterGranted = false;
    std::uint32_t type = 0, version = 0, length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kRecGems) {
            continue;
        }
        if (version < 3) {
            spdlog::warn("[load] 'GEMS' v{} is pre-playable test data — discarded", version);
            continue;
        }
        if (version > kSerVersion) {
            spdlog::error("[load] 'GEMS' v{} is from a NEWER MEO — records preserved as unread, "
                          "sockets inert this session (downgrade unsupported)", version);
            continue;
        }
        a_intfc->ReadRecordData(g_nextUID);
        std::uint8_t starter = 0;
        a_intfc->ReadRecordData(starter);
        g_starterGranted = starter != 0;
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t baseID = 0;
            std::uint16_t uid = 0;
            SocketRecord  rec{};
            std::uint16_t len = 0;
            a_intfc->ReadRecordData(baseID);
            a_intfc->ReadRecordData(uid);
            a_intfc->ReadRecordData(rec.level);
            a_intfc->ReadRecordData(rec.xp);
            a_intfc->ReadRecordData(len);
            rec.gid.resize(len);
            a_intfc->ReadRecordData(rec.gid.data(), len);
            g_sockets[MakeKey(baseID, uid)] = std::move(rec);
        }
    }
    spdlog::info("[load] {} socket record(s), nextUID=0x{:X}, starter={}",
                 g_sockets.size(), g_nextUID, g_starterGranted);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_sockets.clear();
    g_nextUID = 0x9000;
    g_starterGranted = false;
    spdlog::info("[revert] socket index cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        ResolveCatalog();
        ReadConfig();
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        SKSE::GetCrosshairRefEventSource()->AddEventSink(CrosshairSink::GetSingleton());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.7.1 (M3b gem XP) loaded — kills level your socketed gems");
        }
        spdlog::info("kDataLoaded: MEO M3b live; SpellCast + Death + CrosshairRef sinks registered (no code hooks)");
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        // After LoadCallback/Revert — co-save flags are current here.
        SKSE::GetTaskInterface()->AddTask([]() { EnsurePlayerSetup(); });
        break;
    default:
        break;
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.7.1 (M3b kill XP + leveling) loading; runtime {}", gameVersion.string());
    if (gameVersion != REL::Version(1, 6, 1170, 0)) {
        spdlog::warn("Untested runtime {} (built against 1.6.1170)", gameVersion.string());
    }

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(kSerID);
    serialization->SetSaveCallback(SaveCallback);
    serialization->SetLoadCallback(LoadCallback);
    serialization->SetRevertCallback(RevertCallback);

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    spdlog::info("SKSEPluginLoad complete; serialization + messaging registered");
    return true;
}
