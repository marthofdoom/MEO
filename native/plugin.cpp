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
// M4b: EVERY UNIQUE GEM OWNS ITS OWN GEM XP POOL (DESIGN §3). Casting the
// pouch on a weapon holding our gem prompts Unsocket/Swap; unsocketing
// returns THE gem — a real instance (level-correct MISC + ExtraUniqueID +
// the same co-save record, keyed by the gem form) carrying its banked XP.
// The record follows the gem between socketed and loose; the picker lists
// instance gems individually with progress ("Fire II (250/900)").
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
#include <random>
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
constexpr std::uint32_t kSerVersion = 4;  // v4: + mentorGranted flag (v3 reader kept forever)

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
std::vector<int>                                  g_lootableGems;  // resolved, levelable
RE::SpellItem*                                    g_pouchSpell = nullptr;

// ── M3d forms (all IDs extracted from the real Lorerim masters) ───────
constexpr RE::FormID kMentorGemID = 0x8FF;        // MEO.esp MISC (frozen, outside gem range)
constexpr RE::FormID kSoulCairnWorldID = 0x001408;// Dawnguard.esm WRLD DLC01SoulCairn
constexpr RE::FormID kBossLocRefTypeID = 0x0130F7;// Skyrim.esm LCRT "Boss"
constexpr RE::FormID kDragonKeywordID = 0x035D59; // Skyrim.esm KYWD ActorTypeDragon
constexpr RE::FormID kReusableSoulGemKW = 0x0ED2F1;// Skyrim.esm KYWD ReusableSoulGem
RE::TESObjectMISC*      g_mentorGem = nullptr;
RE::TESWorldSpace*      g_soulCairn = nullptr;    // null = Dawnguard absent
RE::BGSLocationRefType* g_bossRefType = nullptr;
RE::BGSKeyword*         g_dragonKeyword = nullptr;
RE::BGSKeyword*         g_reusableSoulGemKW = nullptr;
bool                    g_mentorGranted = false;  // co-save v4
// DESIGN §3 soul-feed Gem XP by soul size (petty..grand; black counts grand).
constexpr float kSoulFeedXP[5] = { 5.0f, 12.0f, 25.0f, 60.0f, 200.0f };

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
    for (std::size_t i = 0; i < g_gems.size(); ++i) {
        if (g_gems[i].mgef && !g_gems[i].def->singleLevel) {
            g_lootableGems.push_back(static_cast<int>(i));
        }
    }
    // M3d forms. Skyrim.esm forms are unconditional; Dawnguard gates Mentor.
    g_mentorGem = dh->LookupForm<RE::TESObjectMISC>(kMentorGemID, kPluginName);
    g_soulCairn = dh->LookupForm<RE::TESWorldSpace>(kSoulCairnWorldID, "Dawnguard.esm");
    g_bossRefType = dh->LookupForm<RE::BGSLocationRefType>(kBossLocRefTypeID, "Skyrim.esm");
    g_dragonKeyword = dh->LookupForm<RE::BGSKeyword>(kDragonKeywordID, "Skyrim.esm");
    g_reusableSoulGemKW = dh->LookupForm<RE::BGSKeyword>(kReusableSoulGemKW, "Skyrim.esm");
    spdlog::info("catalog resolved: {}/{} weapon gems live, {} socketable gem items, pouch={}, "
                 "mentor={}, soulCairn={}, bossType={}",
                 ok, std::size(meo::kWeaponGems), g_gemByItem.size(),
                 g_pouchSpell ? "ok" : "MISSING", g_mentorGem ? "ok" : "MISSING",
                 g_soulCairn ? "ok" : "absent", g_bossRefType ? "ok" : "MISSING");
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

// Worn weapon instance (right hand preferred). Returns false if none drawn.
bool FindWornWeapon(RE::InventoryEntryData*& a_entry, RE::ExtraDataList*& a_xList, bool& a_left) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return false;
    }
    a_entry = nullptr;
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
            if ((right || left) && (!a_entry || (right && a_left))) {
                a_entry = entry;
                a_xList = xList;
                a_left = !right;
            }
        }
    }
    return a_entry != nullptr;
}

// Shared guards; notifies and returns false if the instance can't take a gem.
bool CanSocket(RE::InventoryEntryData* a_entry, RE::ExtraDataList* a_xList) {
    if (a_xList->HasType(RE::ExtraDataType::kEnchantment)) {
        Notify("That weapon already holds a gem.");
        return false;
    }
    if (auto* weap = a_entry->object->As<RE::TESObjectWEAP>(); weap && weap->formEnchanting) {
        Notify("Enchanted weapons cannot hold gems.");
        return false;
    }
    return true;
}

// ── M4b: every unique gem owns its own Gem XP pool (DESIGN §3) ────────
// Unsocketing returns THE gem: a loose gem with banked XP is a real
// instance — level-correct MISC form + ExtraUniqueID + the same co-save
// record shape, keyed (gemItemFormID, uid). The record follows the gem
// between socketed and loose; distinct instances never stack (their extra
// data prevents it). Zero-XP and mastered (V) gems need no record and come
// back as plain stackable items.

// Cumulative Gem XP needed for this gem's NEXT level (0 = none: level V).
float NextThreshold(const meo::GemDef* a_def, int a_level) {
    return (a_level >= 1 && a_level <= 4) ? meo::kXPThresholds[a_level - 1] * a_def->xpMult
                                          : 0.0f;
}

// Hand the player a gem at the given level carrying its banked XP. Engine
// flows only: spawn a real reference, stamp its engine-owned extraList
// (the proven M3c world-weapon recipe), pick it up — extras survive pickup.
void GiveGemInstance(int a_gemIdx, int a_level, float a_xp) {
    auto*       player = RE::PlayerCharacter::GetSingleton();
    const auto& rg = g_gems[a_gemIdx];
    const int   lvIdx = std::clamp(a_level, 1, 5) - 1;
    auto*       gemForm = rg.items[lvIdx];
    if (!player || !gemForm) {
        return;
    }
    if (a_xp <= 0.0f || a_level >= 5) {
        // No progress to preserve: a plain item is behaviorally identical.
        player->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
        return;
    }
    auto ref = player->PlaceObjectAtMe(gemForm, false);
    if (!ref) {
        spdlog::error("[unsock] PlaceObjectAtMe failed for '{}' — plain gem given instead", rg.def->gid);
        player->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
        return;
    }
    auto&               xl = ref->extraList;
    const std::uint16_t uid = g_nextUID++;
    xl.Add(new RE::ExtraUniqueID(gemForm->GetFormID(), uid));
    const std::string name = std::format("{} ({:.0f}/{:.0f})", gemForm->GetName(), a_xp,
                                          NextThreshold(rg.def, a_level));
    auto* xText = new RE::ExtraTextDisplayData(name.c_str());
    xl.Add(xText);
    xText->GetDisplayName(gemForm, 1.0f);  // engine builder reconciles the record
    g_sockets[MakeKey(gemForm->GetFormID(), uid)] =
        SocketRecord{ rg.def->gid, static_cast<std::uint8_t>(lvIdx + 1), a_xp };
    player->PickUpObject(ref.get(), 1, false, false);
    spdlog::info("[unsock] gem instance {:08X}/{}: '{}' as '{}'", gemForm->GetFormID(), uid,
                 rg.def->gid, name);
}

void SocketFromPouch();
void FeedSoul();

// Pull OUR gem out of the worn weapon: engine-remove the enchantment and
// display name (uid stays — harmless), refresh the worn ability, return the
// gem instance with its banked XP. a_thenPicker chains straight into the
// socket picker (the Swap path).
void UnsocketWorn(bool a_thenPicker) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::InventoryEntryData* entry = nullptr;
    RE::ExtraDataList*      xList = nullptr;
    bool                    left = false;
    if (!player || !FindWornWeapon(entry, xList, left)) {
        Notify("Draw the weapon to unsocket its gem.");
        return;
    }
    auto* xid = xList->GetByType<RE::ExtraUniqueID>();
    if (!xid) {
        return;
    }
    const auto key = MakeKey(entry->object->GetFormID(), xid->uniqueID);
    auto       it = g_sockets.find(key);
    if (it == g_sockets.end()) {
        return;
    }
    const SocketRecord rec = it->second;
    auto gemIt = g_gemByGid.find(rec.gid);
    if (gemIt == g_gemByGid.end()) {
        Notify("That gem's essence is missing from this load order.");
        return;
    }
    g_sockets.erase(it);
    xList->RemoveByType(RE::ExtraDataType::kEnchantment);
    xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);
    player->UpdateWeaponAbility(entry->object, xList, left);
    const auto& rg = g_gems[gemIt->second];
    spdlog::info("[unsock] {:08X}/{} released '{}' L{} xp={:.0f}", entry->object->GetFormID(),
                 xid->uniqueID, rec.gid, rec.level, rec.xp);
    GiveGemInstance(gemIt->second, rec.level, rec.xp);
    Notify(std::format("{} {} returned to your pouch.", rg.def->name, meo::kRoman[rec.level - 1]));
    if (a_thenPicker) {
        SKSE::GetTaskInterface()->AddTask([]() { SocketFromPouch(); });
    }
}

class UnsocketPromptCallback : public RE::IMessageBoxCallback {
public:
    void Run(Message a_msg) override {
        const std::int32_t response = static_cast<std::int32_t>(a_msg) - 4;
        if (response == 0) {
            SKSE::GetTaskInterface()->AddTask([]() { UnsocketWorn(false); });
        } else if (response == 1) {
            SKSE::GetTaskInterface()->AddTask([]() { UnsocketWorn(true); });
        } else if (response == 2) {
            SKSE::GetTaskInterface()->AddTask([]() { FeedSoul(); });
        }
        // anything else = Cancel
    }
};

// Factory-created native message box (M4a pattern, verified working).
RE::MessageBoxData* CreateMessageBox() {
    auto* factoryManager = RE::MessageDataFactoryManager::GetSingleton();
    auto* uiStrings = RE::InterfaceStrings::GetSingleton();
    auto* factory = (factoryManager && uiStrings)
                        ? factoryManager->GetCreator<RE::MessageBoxData>(uiStrings->messageBoxData)
                        : nullptr;
    auto* box = factory ? factory->Create() : nullptr;
    if (!box) {
        spdlog::error("MessageBoxData factory unavailable — box skipped");
        return nullptr;
    }
    box->unk4C = 4;
    box->unk38 = 10;
    return box;
}

void ShowUnsocketPrompt(const SocketRecord& a_rec, int a_gemIdx, const char* a_weaponName) {
    auto* box = CreateMessageBox();
    if (!box) {
        return;
    }
    const auto&       rg = g_gems[a_gemIdx];
    const float       need = NextThreshold(rg.def, a_rec.level);
    std::string       gemLabel = std::format("{} {}", rg.def->name, meo::kRoman[a_rec.level - 1]);
    if (need > 0.0f) {
        gemLabel += std::format(" ({:.0f}/{:.0f})", a_rec.xp, need);
    }
    // bodyText is a BSString (owns its copy) — verified from NG source.
    box->bodyText = std::format("{} is socketed in the {}.", gemLabel, a_weaponName).c_str();
    box->buttonText.push_back("Unsocket");
    box->buttonText.push_back("Swap...");
    box->buttonText.push_back("Feed Soul");
    box->buttonText.push_back("Cancel");
    box->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>{ new UnsocketPromptCallback() };
    box->QueueMessage();
}

// ── M4a: native gem-picker message box (chosen over blind first-gem) ──
// Pattern verified against working source (Exit-9B/ForgetSpell,
// D7ry/valhallaCombat): factory-created MessageBoxData, unk4C=4/unk38=10,
// callback receives the pressed button as (Message - 4).
struct GemChoice {
    int                gemIdx;
    int                level;
    RE::TESObjectMISC* item;
    std::int32_t       count;
    // M4b: instance gems (unsocketed with banked XP) are listed individually.
    bool               isInstance = false;
    std::uint16_t      uid = 0;   // gem-side ExtraUniqueID when isInstance
};
std::vector<GemChoice> g_choices;  // captured when the picker opens
constexpr std::size_t  kPickerPageSize = 8;

void SocketChoice(GemChoice a_choice) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::InventoryEntryData* entry = nullptr;
    RE::ExtraDataList*      xList = nullptr;
    bool                    left = false;
    if (!player || !FindWornWeapon(entry, xList, left)) {
        Notify("Draw a weapon to socket a gem.");
        return;
    }
    if (!CanSocket(entry, xList)) {
        return;
    }
    // Re-validate the gem is still in inventory (menus can move things).
    // Instance gems must be re-found by their uid: the record (level + banked
    // XP) moves gem -> weapon, and RemoveItem targets that exact instance.
    RE::ExtraDataList* gemXList = nullptr;
    int                level = a_choice.level;
    float              carriedXP = 0.0f;
    if (a_choice.isInstance) {
        const auto gemKey = MakeKey(a_choice.item->GetFormID(), a_choice.uid);
        auto       recIt = g_sockets.find(gemKey);
        auto*      changes = player->GetInventoryChanges();
        if (recIt != g_sockets.end() && changes && changes->entryList) {
            for (auto* invEntry : *changes->entryList) {
                if (!invEntry || invEntry->object != a_choice.item || !invEntry->extraLists) {
                    continue;
                }
                for (auto* xl : *invEntry->extraLists) {
                    if (!xl) {
                        continue;
                    }
                    if (auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                        xid && xid->uniqueID == a_choice.uid) {
                        gemXList = xl;
                        break;
                    }
                }
            }
        }
        if (!gemXList) {
            Notify("That gem is no longer in your inventory.");
            return;
        }
        level = recIt->second.level;
        carriedXP = recIt->second.xp;
        g_sockets.erase(recIt);
    } else {
        const auto counts = player->GetInventoryCounts(
            [&](RE::TESBoundObject& obj) { return &obj == a_choice.item; });
        if (counts.empty()) {
            Notify("That gem is no longer in your inventory.");
            return;
        }
    }
    const auto uid = StampInstance(entry->object, xList, a_choice.gemIdx, level, carriedXP);
    if (!uid) {
        if (a_choice.isInstance) {  // stamp failed: put the gem record back
            g_sockets[MakeKey(a_choice.item->GetFormID(), a_choice.uid)] =
                SocketRecord{ g_gems[a_choice.gemIdx].def->gid,
                              static_cast<std::uint8_t>(level), carriedXP };
        }
        return;
    }
    player->UpdateWeaponAbility(entry->object, xList, left);
    player->RemoveItem(a_choice.item, 1, RE::ITEM_REMOVE_REASON::kRemove, gemXList, nullptr);
    const auto& rg = g_gems[a_choice.gemIdx];
    Notify(std::format("{} {} socketed into {}.", rg.def->name, meo::kRoman[level - 1],
                       entry->object->GetName()));
}

void ShowGemPicker(std::size_t a_page);

class GemPickerCallback : public RE::IMessageBoxCallback {
public:
    GemPickerCallback(std::size_t a_page, std::size_t a_shown, bool a_more) :
        page(a_page), shown(a_shown), more(a_more) {}

    void Run(Message a_msg) override {
        const std::int32_t response = static_cast<std::int32_t>(a_msg) - 4;
        if (response < 0) {
            return;
        }
        const std::size_t idx = page * kPickerPageSize + static_cast<std::size_t>(response);
        if (static_cast<std::size_t>(response) < shown && idx < g_choices.size()) {
            const GemChoice choice = g_choices[idx];
            SKSE::GetTaskInterface()->AddTask([choice]() { SocketChoice(choice); });
        } else if (more && static_cast<std::size_t>(response) == shown) {
            const std::size_t next = page + 1;
            SKSE::GetTaskInterface()->AddTask([next]() { ShowGemPicker(next); });
        }
        // anything else = Cancel
    }

private:
    std::size_t page;
    std::size_t shown;
    bool        more;
};

void ShowGemPicker(std::size_t a_page) {
    auto* box = CreateMessageBox();
    if (!box) {
        return;
    }
    box->bodyText = "Socket which gem?";

    const std::size_t first = a_page * kPickerPageSize;
    const std::size_t last = std::min(first + kPickerPageSize, g_choices.size());
    for (std::size_t i = first; i < last; ++i) {
        const auto& c = g_choices[i];
        const auto& rg = g_gems[c.gemIdx];
        std::string label = std::format("{} {}", rg.def->name, meo::kRoman[c.level - 1]);
        if (c.isInstance) {
            // A unique gem with banked progress — shown individually (DESIGN §3).
            auto it = g_sockets.find(MakeKey(c.item->GetFormID(), c.uid));
            if (it != g_sockets.end()) {
                label += std::format(" ({:.0f}/{:.0f})", it->second.xp,
                                     NextThreshold(rg.def, it->second.level));
            }
        } else if (c.count > 1) {
            label += std::format(" ({})", c.count);
        }
        box->buttonText.push_back(label.c_str());
    }
    const bool more = last < g_choices.size();
    if (more) {
        box->buttonText.push_back("More...");
    }
    box->buttonText.push_back("Cancel");
    box->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>{
        new GemPickerCallback(a_page, last - first, more)
    };
    box->QueueMessage();
}

void SocketFromPouch() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::InventoryEntryData* entry = nullptr;
    RE::ExtraDataList*      xList = nullptr;
    bool                    left = false;
    if (!player || !FindWornWeapon(entry, xList, left)) {
        Notify("Draw a weapon to socket a gem.");
        return;
    }
    // M4b: pouch-cast on a weapon holding OUR gem = unsocket/swap prompt.
    if (auto* xid = xList->GetByType<RE::ExtraUniqueID>()) {
        const auto key = MakeKey(entry->object->GetFormID(), xid->uniqueID);
        if (auto it = g_sockets.find(key); it != g_sockets.end()) {
            if (auto gemIt = g_gemByGid.find(it->second.gid); gemIt != g_gemByGid.end()) {
                ShowUnsocketPrompt(it->second, gemIt->second, entry->object->GetName());
                return;
            }
        }
    }
    if (!CanSocket(entry, xList)) {
        return;
    }

    // Gather gems: instance gems (banked XP) individually, the rest as
    // stacks; ordered by catalog index then level.
    g_choices.clear();
    auto inv = player->GetInventory([](RE::TESBoundObject& obj) { return obj.Is(RE::FormType::Misc); });
    for (const auto& [obj, data] : inv) {
        if (data.first <= 0) {
            continue;
        }
        auto it = g_gemByItem.find(obj->GetFormID());
        if (it == g_gemByItem.end()) {
            continue;
        }
        auto*        misc = obj->As<RE::TESObjectMISC>();
        std::int32_t instances = 0;
        if (data.second && data.second->extraLists) {
            for (auto* xl : *data.second->extraLists) {
                if (!xl) {
                    continue;
                }
                auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                if (!xid || !g_sockets.contains(MakeKey(obj->GetFormID(), xid->uniqueID))) {
                    continue;
                }
                g_choices.push_back(GemChoice{ it->second.first, it->second.second, misc, 1,
                                               true, xid->uniqueID });
                ++instances;
            }
        }
        if (data.first - instances > 0) {
            g_choices.push_back(GemChoice{ it->second.first, it->second.second, misc,
                                           data.first - instances });
        }
    }
    if (g_choices.empty()) {
        Notify("You have no gems to socket.");
        return;
    }
    std::sort(g_choices.begin(), g_choices.end(), [](const GemChoice& a, const GemChoice& b) {
        return a.gemIdx != b.gemIdx ? a.gemIdx < b.gemIdx : a.level < b.level;
    });
    if (g_choices.size() == 1) {
        SocketChoice(g_choices[0]);  // nothing to choose
        return;
    }
    ShowGemPicker(0);
}

// ── M3b: kill Gem XP -> level-ups -> re-stamp; mastered gems birth ────
// DESIGN §3 (currency is "Gem XP", never "AP"): 1 per player kill to every
// socketed gem on worn weapons; cumulative thresholds in kXPThresholds
// (content-budget calibrated, see BALANCE.md) x the gem's xpMult. Level V =
// mastered: births one level-I copy of itself and stops accruing.
// M3d: boss/dragon kills pay fBossXPMult; follower kills feed the
// follower's own worn gems; soul feeding via the pouch prompt; Mentor Gem
// (doubles the carrier's Gem XP — interim whole-inventory aura until the
// multi-socket model makes it true pairing) granted on Soul Cairn arrival.
// TODO MCM: option to block socket/unsocket while drawn (cosmetic fix for
// FX-mod redraw lag — the real effect applies/removes instantly, verified
// M4b); full version also blocks it in combat. Known issues until then.
float g_xpPerKill = 1.0f;         // [Dev] fXPPerKill in SKSE/Plugins/MEO.ini
float g_gemDropChance = 0.05f;    // [Loot] fGemDropChance — corpse gem on player kill
float g_worldSocketChance = 0.08f;// [Loot] fWorldSocketChance — world weapon born socketed
float g_bossXPMult = 10.0f;       // [XP] fBossXPMult — boss/dragon kill multiplier

void ReadConfig() {
    std::ifstream ini("Data/SKSE/Plugins/MEO.ini");
    std::string   line;
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    };
    while (std::getline(ini, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const float       val = std::strtof(trim(line.substr(eq + 1)).c_str(), nullptr);
        if (key == "fXPPerKill") {
            g_xpPerKill = val;
        } else if (key == "fGemDropChance") {
            g_gemDropChance = val;
        } else if (key == "fWorldSocketChance") {
            g_worldSocketChance = val;
        } else if (key == "fBossXPMult") {
            g_bossXPMult = val;
        }
    }
    if (g_xpPerKill != 1.0f) {
        spdlog::warn("DEV: fXPPerKill={} (MEO.ini override)", g_xpPerKill);
    }
    spdlog::info("config: fGemDropChance={:.2f} fWorldSocketChance={:.2f}",
                 g_gemDropChance, g_worldSocketChance);
}

// Add Gem XP to one socketed worn instance; handles the level-up re-stamp,
// notifications (named for followers), and mastered births. a_rec must be
// the live g_sockets entry (StampInstance rewrites the same key — the
// reference stays valid).
void GrantGemXP(RE::Actor* a_owner, RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                bool a_left, SocketRecord& a_rec, int a_gemIdx, float a_xp,
                std::uint16_t a_uid) {
    const auto& rg = g_gems[a_gemIdx];
    if (!rg.mgef || rg.def->xpMult <= 0.0f || a_rec.level >= 5) {
        return;  // single-level / disabled / mastered gems never level
    }
    a_rec.xp += a_xp;
    const float need = meo::kXPThresholds[a_rec.level - 1] * rg.def->xpMult;
    spdlog::info("[xp] {:08X}/{} {} L{}: {:.0f}/{:.0f}", a_base->GetFormID(), a_uid,
                 a_rec.gid, a_rec.level, a_rec.xp, need);
    if (a_rec.xp < need) {
        return;
    }
    const int   newLevel = a_rec.level + 1;
    const float carriedXP = a_rec.xp;
    StampInstance(a_base, a_xList, a_gemIdx, newLevel, carriedXP);
    a_owner->UpdateWeaponAbility(a_base, a_xList, a_left);
    const bool isPlayer = a_owner->IsPlayerRef();
    const char* who = a_owner->GetName();
    Notify(isPlayer
               ? std::format("Your {} gem has grown to {}.", rg.def->name, meo::kRoman[newLevel - 1])
               : std::format("{}'s {} gem has grown to {}.", who && *who ? who : "Your follower",
                             rg.def->name, meo::kRoman[newLevel - 1]));
    if (newLevel == 5 && rg.items[0]) {
        a_owner->AddObjectToContainer(rg.items[0], nullptr, 1, nullptr);
        Notify(isPlayer ? std::format("Your mastered {} gem births a new gem.", rg.def->name)
                        : std::format("{}'s mastered {} gem births a new gem.",
                                      who && *who ? who : "Your follower", rg.def->name));
        spdlog::info("[birth] mastered '{}' birthed a level-I copy", a_rec.gid);
    }
}

// Award kill Gem XP to every socketed gem on a_owner's worn weapons.
// M3d: followers earn into their own gems (pass the follower); a Mentor Gem
// anywhere in the owner's inventory doubles the gain (interim whole-carrier
// aura — becomes true socket pairing with the multi-socket model).
void AwardKillXP(RE::Actor* a_owner, float a_ap) {
    auto* changes = a_owner ? a_owner->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }
    float xp = a_ap;
    if (g_mentorGem) {
        for (auto* entry : *changes->entryList) {
            if (entry && entry->object == g_mentorGem && entry->countDelta > 0) {
                xp *= 2.0f;
                break;
            }
        }
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
            auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID));
            if (it == g_sockets.end()) {
                continue;
            }
            auto gemIt = g_gemByGid.find(it->second.gid);
            if (gemIt == g_gemByGid.end()) {
                continue;
            }
            GrantGemXP(a_owner, entry->object, xList, left, it->second, gemIt->second, xp,
                       xid->uniqueID);
        }
    }
}

// M3d soul feeding (DESIGN §3): consume the smallest filled soul gem in the
// player's inventory and feed its Gem XP to the worn weapon's gem. Reusable
// soul gems (Azura's Star line, ReusableSoulGem keyword) are never consumed.
void FeedSoul() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::InventoryEntryData* entry = nullptr;
    RE::ExtraDataList*      xList = nullptr;
    bool                    left = false;
    if (!player || !FindWornWeapon(entry, xList, left)) {
        return;
    }
    auto* xid = xList->GetByType<RE::ExtraUniqueID>();
    if (!xid) {
        return;
    }
    auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID));
    if (it == g_sockets.end()) {
        return;
    }
    auto gemIt = g_gemByGid.find(it->second.gid);
    if (gemIt == g_gemByGid.end()) {
        return;
    }
    if (it->second.level >= 5) {
        Notify("A mastered gem can grow no further.");
        return;
    }
    // Smallest filled soul first (waste-free default). A stack's fill comes
    // from the base form; player-filled gems carry ExtraSoul per instance.
    RE::TESBoundObject* bestItem = nullptr;
    RE::ExtraDataList*  bestXL = nullptr;
    int                 bestLevel = 99;
    auto inv = player->GetInventory([](RE::TESBoundObject& obj) { return obj.Is(RE::FormType::SoulGem); });
    for (const auto& [obj, data] : inv) {
        if (data.first <= 0) {
            continue;
        }
        auto* soulGem = obj->As<RE::TESSoulGem>();
        if (!soulGem || (g_reusableSoulGemKW && soulGem->HasKeyword(g_reusableSoulGemKW))) {
            continue;
        }
        std::int32_t withExtras = 0;
        if (data.second && data.second->extraLists) {
            for (auto* xl : *data.second->extraLists) {
                if (!xl) {
                    continue;
                }
                ++withExtras;
                auto* xSoul = xl->GetByType<RE::ExtraSoul>();
                const int lv = xSoul ? static_cast<int>(xSoul->GetContainedSoul()) : 0;
                if (lv >= 1 && lv <= 5 && lv < bestLevel) {
                    bestLevel = lv;
                    bestItem = obj;
                    bestXL = xl;
                }
            }
        }
        const int baseLv = static_cast<int>(soulGem->GetContainedSoul());
        if (data.first - withExtras > 0 && baseLv >= 1 && baseLv <= 5 && baseLv < bestLevel) {
            bestLevel = baseLv;
            bestItem = obj;
            bestXL = nullptr;
        }
    }
    if (!bestItem) {
        Notify("You have no filled soul gems.");
        return;
    }
    const float xp = kSoulFeedXP[bestLevel - 1];
    const auto& rg = g_gems[gemIt->second];
    spdlog::info("[feed] soul L{} (+{:.0f}) -> '{}' on {:08X}/{}", bestLevel, xp,
                 it->second.gid, entry->object->GetFormID(), xid->uniqueID);
    GrantGemXP(player, entry->object, xList, left, it->second, gemIt->second, xp,
               xid->uniqueID);
    player->RemoveItem(bestItem, 1, RE::ITEM_REMOVE_REASON::kRemove, bestXL, nullptr);
    Notify(std::format("The soul feeds your {} gem (+{:.0f} Gem XP).", rg.def->name, xp));
}

// ── M3c: gems enter the world — corpse drops + weapons born socketed ──
// No leveled-list surgery: player kills roll a corpse gem (lootable), and
// world weapon REFERENCES roll a pre-socketed gem the moment their cell
// attaches (TESCellAttachDetachEvent fires PER REFERENCE). World rolls are
// hashed from the refID, so a given reference decides once, forever —
// stable across revisits without storing anything.
std::mt19937 g_rng{ std::random_device{}() };

std::uint32_t HashU32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

void RollCorpseGem(RE::Actor* a_victim) {
    if (g_lootableGems.empty() || g_gemDropChance <= 0.0f) {
        return;
    }
    if (std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) >= g_gemDropChance) {
        return;
    }
    const auto& rg = g_gems[g_lootableGems[
        std::uniform_int_distribution<std::size_t>(0, g_lootableGems.size() - 1)(g_rng)]];
    if (rg.items[0]) {
        a_victim->AddObjectToContainer(rg.items[0], nullptr, 1, nullptr);
        spdlog::info("[loot] corpse gem '{}' I on {:08X}", rg.def->gid, a_victim->GetFormID());
    }
}

void MaybeStampWorldWeapon(RE::TESObjectREFR* a_ref) {
    auto* base = a_ref->GetBaseObject();
    if (!base || !base->Is(RE::FormType::Weapon) || g_lootableGems.empty() ||
        g_worldSocketChance <= 0.0f) {
        return;
    }
    if (a_ref->IsDeleted() || a_ref->IsDisabled()) {
        return;
    }
    auto& xList = a_ref->extraList;
    if (xList.HasType(RE::ExtraDataType::kEnchantment)) {
        return;  // already socketed (ours or engine)
    }
    if (auto* weap = base->As<RE::TESObjectWEAP>(); weap && weap->formEnchanting) {
        return;  // base-enchanted
    }
    if (auto* xid = xList.GetByType<RE::ExtraUniqueID>();
        xid && g_sockets.contains(MakeKey(base->GetFormID(), xid->uniqueID))) {
        return;  // recorded already
    }
    // Deterministic per-reference roll (same verdict on every cell attach).
    const std::uint32_t h = HashU32(a_ref->GetFormID() ^ 0x4D454F31u);  // 'MEO1'
    if ((h % 10000) / 10000.0f >= g_worldSocketChance) {
        return;
    }
    const int gemIdx = g_lootableGems[HashU32(h) % g_lootableGems.size()];
    const int level = (HashU32(h ^ 0x5A5A5A5Au) % 100) < 85 ? 1 : 2;  // mostly I, some II
    if (StampInstance(base, &xList, gemIdx, level)) {
        spdlog::info("[world] ref {:08X} born socketed: {} {} {}", a_ref->GetFormID(),
                     g_gems[gemIdx].def->name, meo::kRoman[level - 1], base->GetName());
    }
}

class CellAttachSink : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
public:
    static CellAttachSink* GetSingleton() {
        static CellAttachSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
                                          RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
        if (!a_event || !a_event->attached || !a_event->reference) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (a_event->reference->GetBaseObject() &&
            a_event->reference->GetBaseObject()->Is(RE::FormType::Weapon)) {
            const RE::ObjectRefHandle handle = a_event->reference->GetHandle();
            SKSE::GetTaskInterface()->AddTask([handle]() {
                if (auto ref = handle.get()) {
                    MaybeStampWorldWeapon(ref.get());
                }
            });
        }
        // M3d: the Mentor Gem waits in the Soul Cairn (mid-Dawnguard, between
        // Chasing Echoes and Beyond Death) — granted once when the player
        // arrives. Attach events for Soul Cairn refs gate the check for free.
        if (!g_mentorGranted && g_mentorGem && g_soulCairn &&
            a_event->reference->GetWorldspace() == g_soulCairn) {
            SKSE::GetTaskInterface()->AddTask([]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (g_mentorGranted || !player || player->GetWorldspace() != g_soulCairn) {
                    return;
                }
                g_mentorGranted = true;  // persisted in the co-save (v4)
                player->AddObjectToContainer(g_mentorGem, nullptr, 1, nullptr);
                Notify("A resonance among the souls draws to you... a Mentor Gem joins your pouch.");
                spdlog::info("[mentor] granted on Soul Cairn arrival");
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent> {
public:
    static DeathSink* GetSingleton() {
        static DeathSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
                                          RE::BSTEventSource<RE::TESDeathEvent>*) override {
        if (!a_event || !a_event->dead || !a_event->actorDying ||
            a_event->actorDying->IsPlayerRef() || !a_event->actorKiller) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // M3d: player kills AND follower kills (followers feed their own gems).
        auto* killer = a_event->actorKiller->As<RE::Actor>();
        if (!killer || (!killer->IsPlayerRef() && !killer->IsPlayerTeammate())) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // M3d: bosses (location ref type "Boss") and dragons pay fBossXPMult.
        float mult = 1.0f;
        auto* victimRef = a_event->actorDying.get();
        if (auto* xlrt = victimRef->extraList.GetByType<RE::ExtraLocationRefType>();
            g_bossRefType && xlrt && xlrt->locRefType == g_bossRefType) {
            mult = g_bossXPMult;
        } else if (auto* victimActor = victimRef->As<RE::Actor>();
                   g_dragonKeyword && victimActor && victimActor->GetRace() &&
                   victimActor->GetRace()->HasKeyword(g_dragonKeyword)) {
            mult = g_bossXPMult;
        }
        if (mult > 1.0f) {
            spdlog::info("[xp] boss/dragon kill: x{:.0f}", mult);
        }
        const bool                fromPlayer = killer->IsPlayerRef();
        const RE::ObjectRefHandle victim = a_event->actorDying->GetHandle();
        const RE::ObjectRefHandle killerHandle = a_event->actorKiller->GetHandle();
        const float               xp = g_xpPerKill * mult;
        SKSE::GetTaskInterface()->AddTask([victim, killerHandle, xp, fromPlayer]() {
            if (auto k = killerHandle.get()) {
                if (auto* killerActor = k->As<RE::Actor>()) {
                    AwardKillXP(killerActor, xp);
                }
            }
            if (fromPlayer) {  // corpse gems stay player-kill only
                if (auto ref = victim.get()) {
                    if (auto* v = ref->As<RE::Actor>()) {
                        RollCorpseGem(v);
                    }
                }
            }
        });
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
    const std::uint8_t mentor = g_mentorGranted ? 1 : 0;  // v4 field
    a_intfc->WriteRecordData(mentor);
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
    spdlog::info("[save] {} socket record(s), nextUID=0x{:X}, starter={}, mentor={}",
                 g_sockets.size(), g_nextUID, g_starterGranted, g_mentorGranted);
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_sockets.clear();
    g_nextUID = 0x9000;
    g_starterGranted = false;
    g_mentorGranted = false;
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
        if (version >= 4) {  // v4: mentorGranted (v3 saves default to false)
            std::uint8_t mentor = 0;
            a_intfc->ReadRecordData(mentor);
            g_mentorGranted = mentor != 0;
        }
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
    spdlog::info("[load] {} socket record(s), nextUID=0x{:X}, starter={}, mentor={}",
                 g_sockets.size(), g_nextUID, g_starterGranted, g_mentorGranted);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_sockets.clear();
    g_nextUID = 0x9000;
    g_starterGranted = false;
    g_mentorGranted = false;
    spdlog::info("[revert] socket index cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        ResolveCatalog();
        ReadConfig();
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellAttachDetachEvent>(CellAttachSink::GetSingleton());
        SKSE::GetCrosshairRefEventSource()->AddEventSink(CrosshairSink::GetSingleton());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.10.0 (M3d) loaded — bosses, followers, souls, the Mentor");
        }
        spdlog::info("kDataLoaded: MEO M3d live; SpellCast + Death + CellAttach + CrosshairRef sinks registered (no code hooks)");
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
    spdlog::info("MEO native v0.10.0 (M3d: boss XP, follower gems, soul feeding, Mentor Gem) loading; runtime {}", gameVersion.string());
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
