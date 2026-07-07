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
// The record follows the gem between socketed and loose; instance gems are
// listed individually with progress ("Fire II (250/900)").
//
// M5: THE GEM POUCH IS A REAL CONTAINER MENU. The message-box menus are
// gone; the pouch power opens a hidden container (CONT 0x8FE) as a native
// two-pane ContainerMenu. Take the shown gem out = unsocket; put a gem in
// = socket (atomic swap when one is already in); drop filled soul gems in
// = feed. One reconcile pass on menu close does all mutation.
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
constexpr RE::FormID kPouchContID = 0x8FE;        // MEO.esp CONT (frozen) — M5 Gem Pouch menu
constexpr RE::FormID kMentorGemID = 0x8FF;        // MEO.esp MISC (frozen, outside gem range)
constexpr RE::FormID kSoulCairnWorldID = 0x001408;// Dawnguard.esm WRLD DLC01SoulCairn
constexpr RE::FormID kBossLocRefTypeID = 0x0130F7;// Skyrim.esm LCRT "Boss"
constexpr RE::FormID kDragonKeywordID = 0x035D59; // Skyrim.esm KYWD ActorTypeDragon
constexpr RE::FormID kReusableSoulGemKW = 0x0ED2F1;// Skyrim.esm KYWD ReusableSoulGem
RE::TESObjectCONT*      g_pouchCont = nullptr;
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
    g_pouchCont = dh->LookupForm<RE::TESObjectCONT>(kPouchContID, kPluginName);
    if (!g_pouchCont) {
        spdlog::error("Gem Pouch container 0x{:X} not found in {} — ESP older than the DLL?",
                      kPouchContID, kPluginName);
    }
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
bool  g_xpNotify = true;          // [UI] bXPNotify — "Gem XP +N" on kills (MCM later)

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
        } else if (key == "bXPNotify") {
            g_xpNotify = val != 0.0f;
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
bool GrantGemXP(RE::Actor* a_owner, RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                bool a_left, SocketRecord& a_rec, int a_gemIdx, float a_xp,
                std::uint16_t a_uid) {
    const auto& rg = g_gems[a_gemIdx];
    if (!rg.mgef || rg.def->xpMult <= 0.0f || a_rec.level >= 5) {
        return false;  // single-level / disabled / mastered gems never level
    }
    a_rec.xp += a_xp;
    const float need = meo::kXPThresholds[a_rec.level - 1] * rg.def->xpMult;
    spdlog::info("[xp] {:08X}/{} {} L{}: {:.0f}/{:.0f}", a_base->GetFormID(), a_uid,
                 a_rec.gid, a_rec.level, a_rec.xp, need);
    if (a_rec.xp < need) {
        return true;
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
    return true;
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
    int awarded = 0;
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
            if (GrantGemXP(a_owner, entry->object, xList, left, it->second, gemIt->second, xp,
                           xid->uniqueID)) {
                ++awarded;
            }
        }
    }
    // "Gem XP +N" HUD feedback (player only; bXPNotify, MCM toggle later).
    if (awarded > 0 && g_xpNotify && a_owner->IsPlayerRef()) {
        Notify(awarded == 1 ? std::format("Gem XP +{:.0f}", xp)
                            : std::format("Gem XP +{:.0f} (x{} gems)", xp, awarded));
    }
}

// ── M5: the Gem Pouch is a real two-pane ContainerMenu ────────────────
// Casting the pouch power spawns a temporary hidden container ref (CONT
// 0x8FE, no model) at the player and activates it — the engine's own
// container UI supplies listing, paging, item cards and cancel semantics
// the M4a/M4b message boxes never could (and whose Swap button was
// non-atomic: the old gem left the weapon before a new one was chosen).
// All mutation happens in ONE reconcile pass when the menu closes:
//   - the socketed gem sits in the pouch as a live instance; TAKE IT OUT
//     to unsocket — the item in hand becomes the banked-XP gem,
//   - PUT A GEM IN to socket it; with one already socketed that is an
//     atomic swap (old gem lands in your inventory, new one in the weapon),
//   - PUT FILLED SOUL GEMS IN to feed the socketed gem, smallest first
//     (reusable gems and everything non-gem bounce back untouched).
// The pouch ref is deleted after reconcile. A dangling session (activation
// never opened a menu) self-heals on the next ContainerMenu close. Known
// edge (documented): saving with the pouch OPEN persists the temp ref and
// the display copy — harmless but untidy; don't save inside the pouch.

struct PouchSession {
    bool                active = false;
    RE::ObjectRefHandle pouchRef;
    RE::FormID          weaponBase = 0;   // worn weapon at open time
    std::uint16_t       weaponUID = 0;    // 0 = weapon had no socket
    RE::TESObjectMISC*  displayForm = nullptr;  // in-pouch copy of the socketed gem
    std::uint16_t       displayUID = 0;
};
PouchSession g_pouch;

// Live ExtraDataList of instance (a_form, a_uid) in a_owner's inventory
// (the proven M4b re-find, factored out).
RE::ExtraDataList* FindInstanceXList(RE::TESObjectREFR* a_owner, RE::TESBoundObject* a_form,
                                     std::uint16_t a_uid) {
    auto* changes = a_owner ? a_owner->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return nullptr;
    }
    for (auto* entry : *changes->entryList) {
        if (!entry || entry->object != a_form || !entry->extraLists) {
            continue;
        }
        for (auto* xl : *entry->extraLists) {
            if (!xl) {
                continue;
            }
            if (auto* xid = xl->GetByType<RE::ExtraUniqueID>(); xid && xid->uniqueID == a_uid) {
                return xl;
            }
        }
    }
    return nullptr;
}

void OpenGemPouch() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::InventoryEntryData* entry = nullptr;
    RE::ExtraDataList*      xList = nullptr;
    bool                    left = false;
    if (!player || !FindWornWeapon(entry, xList, left)) {
        Notify("Draw a weapon to open the Gem Pouch.");
        return;
    }
    if (!g_pouchCont) {
        spdlog::error("[pouch] container form missing — MEO.esp older than the DLL?");
        Notify("The Gem Pouch is missing — update MEO.esp.");
        return;
    }
    if (g_pouch.active) {
        spdlog::warn("[pouch] session already active — ignored");
        return;
    }
    // A weapon holding OUR gem opens the pouch showing it; otherwise the
    // weapon must be socketable (CanSocket notifies why not).
    std::uint16_t       wuid = 0;
    const SocketRecord* rec = nullptr;
    if (auto* xid = xList->GetByType<RE::ExtraUniqueID>()) {
        if (auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID));
            it != g_sockets.end()) {
            wuid = xid->uniqueID;
            rec = &it->second;
        }
    }
    if (!rec && !CanSocket(entry, xList)) {
        return;
    }
    auto pouch = player->PlaceObjectAtMe(g_pouchCont, false);
    if (!pouch) {
        spdlog::error("[pouch] PlaceObjectAtMe failed");
        return;
    }
    g_pouch = PouchSession{ true, pouch->GetHandle(), entry->object->GetFormID(), wuid };

    // The socketed gem appears inside as a live instance: the M4b
    // spawn-stamp-pickup recipe, then moved into the container. No socket
    // record yet — it only becomes "the gem" if the player takes it.
    if (rec) {
        if (auto gemIt = g_gemByGid.find(rec->gid); gemIt != g_gemByGid.end()) {
            const auto& rg = g_gems[gemIt->second];
            const int   lvIdx = std::clamp<int>(rec->level, 1, 5) - 1;
            auto*       gemForm = rg.items[lvIdx];
            auto        gemRef = gemForm ? player->PlaceObjectAtMe(gemForm, false)
                                         : RE::NiPointer<RE::TESObjectREFR>{};
            if (gemRef) {
                const std::uint16_t uid = g_nextUID++;
                gemRef->extraList.Add(new RE::ExtraUniqueID(gemForm->GetFormID(), uid));
                const float       need = NextThreshold(rg.def, rec->level);
                const std::string name =
                    need > 0.0f
                        ? std::format("{} ({:.0f}/{:.0f})", gemForm->GetName(), rec->xp, need)
                        : std::format("{} (mastered)", gemForm->GetName());
                auto* xText = new RE::ExtraTextDisplayData(name.c_str());
                gemRef->extraList.Add(xText);
                xText->GetDisplayName(gemForm, 1.0f);  // engine builder reconcile
                player->PickUpObject(gemRef.get(), 1, false, false);
                if (auto* gemXL = FindInstanceXList(player, gemForm, uid)) {
                    player->RemoveItem(gemForm, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                       gemXL, pouch.get());
                    g_pouch.displayForm = gemForm;
                    g_pouch.displayUID = uid;
                } else {
                    spdlog::error("[pouch] display instance lost after pickup (uid=0x{:X})", uid);
                }
            }
        }
    }
    pouch->ActivateRef(player, 0, nullptr, 1, false);
    spdlog::info("[pouch] opened: weapon {:08X}/{} display={:08X}/0x{:X}", g_pouch.weaponBase,
                 wuid, g_pouch.displayForm ? g_pouch.displayForm->GetFormID() : 0,
                 g_pouch.displayUID);
}

// One pass over whatever the player left in the pouch, run when the menu
// closes. The session is consumed up front so every path ends clean.
void ReconcilePouch() {
    if (!g_pouch.active) {
        return;
    }
    const PouchSession s = g_pouch;
    g_pouch = PouchSession{};
    auto  pouchPtr = s.pouchRef.get();
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!pouchPtr || !player) {
        return;
    }
    auto* pouch = pouchPtr.get();

    // Snapshot the pouch: our gems, feedable souls, everything else.
    struct Deposit {
        RE::TESBoundObject* obj;
        std::int32_t        count;   // >1 only for plain stacks (xl null)
        RE::ExtraDataList*  xl;
        std::uint16_t       uid;     // gem instances (0 = plain)
        int                 soulLv;  // souls only
    };
    bool                 displayHere = false;
    RE::ExtraDataList*   displayXL = nullptr;
    std::vector<Deposit> gems, souls, other;
    for (const auto& [obj, data] : pouch->GetInventory()) {
        if (data.first <= 0) {
            continue;
        }
        const bool isGem = g_gemByItem.contains(obj->GetFormID());
        auto*      soulGem = obj->As<RE::TESSoulGem>();
        const bool feedable =
            soulGem && !(g_reusableSoulGemKW && soulGem->HasKeyword(g_reusableSoulGemKW));
        std::int32_t plain = data.first;
        if (data.second && data.second->extraLists) {
            for (auto* xl : *data.second->extraLists) {
                if (!xl) {
                    continue;
                }
                const std::int32_t n = std::max(xl->GetCount(), 1);
                plain -= n;
                if (isGem) {
                    auto*               xid = xl->GetByType<RE::ExtraUniqueID>();
                    const std::uint16_t uid = xid ? xid->uniqueID : 0;
                    if (obj == s.displayForm && uid == s.displayUID) {
                        displayHere = true;
                        displayXL = xl;
                    } else {
                        gems.push_back({ obj, n, xl, uid, 0 });
                    }
                } else if (feedable) {
                    auto*     xSoul = xl->GetByType<RE::ExtraSoul>();
                    const int lv = static_cast<int>(xSoul ? xSoul->GetContainedSoul()
                                                          : soulGem->GetContainedSoul());
                    if (lv >= 1 && lv <= 5) {
                        souls.push_back({ obj, n, xl, 0, lv });
                    } else {
                        other.push_back({ obj, n, xl, 0, 0 });
                    }
                } else {
                    other.push_back({ obj, n, xl, 0, 0 });
                }
            }
        }
        if (plain > 0) {
            const int baseLv = soulGem ? static_cast<int>(soulGem->GetContainedSoul()) : 0;
            if (isGem) {
                gems.push_back({ obj, plain, nullptr, 0, 0 });
            } else if (feedable && baseLv >= 1 && baseLv <= 5) {
                souls.push_back({ obj, plain, nullptr, 0, baseLv });
            } else {
                other.push_back({ obj, plain, nullptr, 0, 0 });
            }
        }
    }

    // Where is the session weapon now? The container menu allows equipping,
    // so the player may have swapped weapons while it was open.
    auto* sessionForm = RE::TESForm::LookupByID<RE::TESBoundObject>(s.weaponBase);
    auto* sessionXL = (sessionForm && s.weaponUID)
                          ? FindInstanceXList(player, sessionForm, s.weaponUID)
                          : nullptr;

    // Unsocket the session weapon; its record binds to the (ex-)display gem
    // instance, which by then is in the player's hands (M4b flow).
    auto unsocketWeapon = [&]() -> bool {
        auto it = g_sockets.find(MakeKey(s.weaponBase, s.weaponUID));
        if (it == g_sockets.end() || !s.displayForm) {
            return false;
        }
        if (!sessionXL) {
            spdlog::warn("[pouch] session weapon {:08X}/{} not found — record kept",
                         s.weaponBase, s.weaponUID);
            return false;
        }
        const SocketRecord rec = it->second;
        g_sockets.erase(it);
        sessionXL->RemoveByType(RE::ExtraDataType::kEnchantment);
        sessionXL->RemoveByType(RE::ExtraDataType::kTextDisplayData);
        const bool wr = sessionXL->HasType(RE::ExtraDataType::kWorn);
        const bool wl = sessionXL->HasType(RE::ExtraDataType::kWornLeft);
        if (wr || wl) {
            player->UpdateWeaponAbility(sessionForm, sessionXL, wl);
        }
        if (rec.xp > 0.0f && rec.level < 5) {
            g_sockets[MakeKey(s.displayForm->GetFormID(), s.displayUID)] = rec;
        } else if (auto* gemXL = FindInstanceXList(player, s.displayForm, s.displayUID)) {
            // No progress worth banking — normalize to a plain stackable gem.
            gemXL->RemoveByType(RE::ExtraDataType::kUniqueID);
            gemXL->RemoveByType(RE::ExtraDataType::kTextDisplayData);
        }
        spdlog::info("[pouch] unsocketed {:08X}/{}: '{}' L{} xp={:.0f}", s.weaponBase,
                     s.weaponUID, rec.gid, rec.level, rec.xp);
        return true;
    };

    // 1. Display gem taken out = unsocket.
    if (s.weaponUID && s.displayForm && !displayHere) {
        if (unsocketWeapon()) {
            Notify("Gem unsocketed.");
        }
    }

    RE::InventoryEntryData* wEntry = nullptr;
    RE::ExtraDataList*      wXL = nullptr;
    bool                    wLeft = false;
    const bool              wornFound = FindWornWeapon(wEntry, wXL, wLeft);

    // 2. Deposited gems: the first (lowest catalog index/level) socketes
    // into the worn weapon; everything else bounces back.
    if (!gems.empty()) {
        std::sort(gems.begin(), gems.end(), [](const Deposit& a, const Deposit& b) {
            const auto pa = g_gemByItem.find(a.obj->GetFormID())->second;
            const auto pb = g_gemByItem.find(b.obj->GetFormID())->second;
            return pa != pb ? pa < pb : a.uid < b.uid;
        });
        Deposit& d = gems.front();
        bool     socketed = false;
        if (wornFound) {
            // Atomic swap: the displayed old gem goes to the player FIRST;
            // only then does anything leave the weapon.
            if (displayHere && wXL == sessionXL) {
                pouch->RemoveItem(s.displayForm, 1, RE::ITEM_REMOVE_REASON::kRemove, displayXL,
                                  player);
                displayHere = false;
                displayXL = nullptr;
                unsocketWeapon();
            }
            auto* weap = wEntry->object->As<RE::TESObjectWEAP>();
            if (!wXL->HasType(RE::ExtraDataType::kEnchantment) && (!weap || !weap->formEnchanting)) {
                const auto [gemIdx, itemLevel] = g_gemByItem.find(d.obj->GetFormID())->second;
                int          level = itemLevel;
                float        xp = 0.0f;
                bool         hadRec = false;
                SocketRecord saved{};
                if (d.uid) {
                    if (auto it = g_sockets.find(MakeKey(d.obj->GetFormID(), d.uid));
                        it != g_sockets.end()) {
                        saved = it->second;
                        hadRec = true;
                        level = saved.level;
                        xp = saved.xp;
                        g_sockets.erase(it);
                    } else {
                        spdlog::warn("[pouch] deposited instance uid=0x{:X} has no record — "
                                     "socketed fresh", d.uid);
                    }
                }
                if (StampInstance(wEntry->object, wXL, gemIdx, level, xp)) {
                    player->UpdateWeaponAbility(wEntry->object, wXL, wLeft);
                    pouch->RemoveItem(d.obj, 1, RE::ITEM_REMOVE_REASON::kRemove, d.xl, nullptr);
                    d.count -= 1;
                    const auto& rg = g_gems[gemIdx];
                    Notify(std::format("{} {} socketed into {}.", rg.def->name,
                                       meo::kRoman[level - 1], wEntry->object->GetName()));
                    socketed = true;
                } else if (hadRec) {
                    g_sockets[MakeKey(d.obj->GetFormID(), d.uid)] = saved;
                }
            }
        }
        if (!socketed) {
            Notify(wornFound ? "That weapon cannot take a gem — returned."
                             : "Draw a weapon to socket a gem — returned.");
        }
        std::int32_t extras = 0;
        for (auto& g : gems) {
            if (g.count > 0) {
                pouch->RemoveItem(g.obj, g.count, RE::ITEM_REMOVE_REASON::kRemove, g.xl, player);
                extras += g.count;
            }
        }
        if (socketed && extras > 0) {
            Notify("One gem per socket — the extra gems were returned.");
        }
    }

    // 3. Souls feed whatever is socketed NOW (post-swap), smallest first.
    if (!souls.empty()) {
        auto* xid = wornFound ? wXL->GetByType<RE::ExtraUniqueID>() : nullptr;
        auto  it = xid ? g_sockets.find(MakeKey(wEntry->object->GetFormID(), xid->uniqueID))
                       : g_sockets.end();
        auto  gemIt = it != g_sockets.end() ? g_gemByGid.find(it->second.gid) : g_gemByGid.end();
        if (gemIt != g_gemByGid.end()) {
            std::sort(souls.begin(), souls.end(),
                      [](const Deposit& a, const Deposit& b) { return a.soulLv < b.soulLv; });
            int   fed = 0;
            float total = 0.0f;
            for (auto& d : souls) {
                while (d.count > 0 && it->second.level < 5) {
                    const float xp = kSoulFeedXP[d.soulLv - 1];
                    spdlog::info("[feed] soul L{} (+{:.0f}) -> '{}'", d.soulLv, xp, it->second.gid);
                    GrantGemXP(player, wEntry->object, wXL, wLeft, it->second, gemIt->second, xp,
                               xid->uniqueID);
                    pouch->RemoveItem(d.obj, 1, RE::ITEM_REMOVE_REASON::kRemove, d.xl, nullptr);
                    d.count -= 1;
                    total += xp;
                    ++fed;
                }
            }
            if (fed > 0) {
                Notify(std::format("{} soul(s) fed to the gem (+{:.0f} Gem XP).", fed, total));
            } else {
                Notify("A mastered gem can grow no further — soul gems returned.");
            }
        } else {
            Notify("No socketed gem to feed — soul gems returned.");
        }
        for (auto& d : souls) {
            if (d.count > 0) {
                pouch->RemoveItem(d.obj, d.count, RE::ITEM_REMOVE_REASON::kRemove, d.xl, player);
            }
        }
    }

    // 4. Everything else bounces; an untouched display copy is deleted
    // (the weapon keeps its gem); the pouch ref goes away.
    for (auto& d : other) {
        pouch->RemoveItem(d.obj, d.count, RE::ITEM_REMOVE_REASON::kRemove, d.xl, player);
    }
    if (!other.empty()) {
        Notify("Only gems and filled soul gems belong in the pouch.");
    }
    if (displayHere) {
        pouch->RemoveItem(s.displayForm, 1, RE::ITEM_REMOVE_REASON::kRemove, displayXL, nullptr);
    }
    pouch->SetDelete(true);
    spdlog::info("[pouch] reconciled: display={} gems={} souls={} other={}",
                 displayHere ? "kept" : (s.displayForm ? "taken" : "none"), gems.size(),
                 souls.size(), other.size());
}

// ContainerMenu close while our session is live -> reconcile (task-deferred).
class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuSink* GetSingleton() {
        static MenuSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (a_event && !a_event->opening && g_pouch.active &&
            a_event->menuName == RE::ContainerMenu::MENU_NAME) {
            SKSE::GetTaskInterface()->AddTask([]() { ReconcilePouch(); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

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
            SKSE::GetTaskInterface()->AddTask([]() { OpenGemPouch(); });
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
    g_pouch = PouchSession{};
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
    g_pouch = PouchSession{};
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
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.11.0 (M5 Gem Pouch menu) loaded");
        }
        spdlog::info("kDataLoaded: MEO M5 live; SpellCast + Death + CellAttach + CrosshairRef + MenuOpenClose sinks registered (no code hooks)");
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
    spdlog::info("MEO native v0.11.0 (M5: Gem Pouch container menu) loading; runtime {}", gameVersion.string());
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
