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
// M6: NATIVE ImGui GEM MENU (the M5 ContainerMenu was scrapped same-day:
// the engine REWRITES ExtraUniqueID on container transfer, orphaning
// banked-XP records — see ENGINE_NOTES §1). The pouch power now opens an
// ImGui overlay drawn from a DXGI-present hook: left pane lists every
// socketable weapon instance, right pane lists loose gems; selecting an
// item highlights its socketed gem (click = unsocket) and clicking another
// gem socketes/swaps ATOMICALLY. Gems never leave the player's inventory,
// so instance identity stays stable (the M4b-proven flows). These are
// MEO's first code hooks — render + input only, no gameplay hooks; the
// pattern is copied from D7ry/wheeler (proven on AE).
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

// M6 ImGui menu (render/input hooks). d3d11.h pulls windows.h — NG itself
// never includes it, so guard against the min/max macros ourselves.
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

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
// key = (baseFormID << 24) | (uniqueID << 8) | slot  — multi-socket (m13):
// one item instance (base,uid) can hold up to kMaxSockets gems, one per slot.
// Loose gem records always use slot 0. See save-safety rule 3.
constexpr int kMaxSockets = 2;
using InstKey = std::uint64_t;
constexpr InstKey MakeKey(RE::FormID a_base, std::uint16_t a_uid, std::uint8_t a_slot = 0) {
    return (static_cast<InstKey>(a_base) << 24) | (static_cast<InstKey>(a_uid) << 8) | a_slot;
}
std::unordered_map<InstKey, SocketRecord> g_sockets;
std::uint16_t g_nextUID = 0x9000;  // our range, clear of engine-assigned ids
bool          g_starterGranted = false;

constexpr std::uint32_t kSerID = 'MEO1';
constexpr std::uint32_t kRecGems = 'GEMS';
constexpr std::uint32_t kSerVersion = 6;  // v6: + armorStarterGranted. v5: per-socket slot. v3/v4 → slot 0

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
std::vector<int>                                  g_lootableGems;  // weapon-domain, world-weapon stamps
std::vector<int>                                  g_corpseGems;    // weapon+armor, corpse drops
RE::SpellItem*                                    g_pouchSpell = nullptr;
float g_magnitudeMult = 1.0f;  // [XP] fMagnitudeMult master power scale; used by StampInstance below, set by ReadConfig (MCM)

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
bool                    g_armorStarterGranted = false;  // co-save v6
// DESIGN §3 soul-feed Gem XP by soul size (petty..grand; black counts grand).
constexpr float kSoulFeedXP[5] = { 5.0f, 12.0f, 25.0f, 60.0f, 200.0f };
// Filled vanilla soul gems (Skyrim.esm), petty..grand — gem destruction
// reclaims 1/10 of banked Gem XP into the largest of these it can afford.
constexpr RE::FormID kFilledSoulGemIDs[5] = { 0x02E4E3, 0x02E4E5, 0x02E4F3, 0x02E4FB, 0x02E4FF };
RE::TESSoulGem*      g_filledSoulGems[5] = {};

// MEO perks (DESIGN §6). MEO.esp-local FormIDs 0x810.. — see MEO_GenerateESP.
constexpr RE::FormID kPerkAttuneBase = 0x810;  // 0x810..0x814 = Attunement 1..5
constexpr RE::FormID kPerkGemCutter  = 0x815;
constexpr RE::FormID kPerkSoulFeeder = 0x816;
RE::BGSPerk* g_perkAttune[5] = {};
RE::BGSPerk* g_perkGemCutter = nullptr;
RE::BGSPerk* g_perkSoulFeeder = nullptr;
// Cached from the player's perks (refreshed on load + menu close).
int  g_attuneRank = 0;      // 0..5 → +8% gem magnitude per rank
bool g_hasGemCutter = false;  // +50% Gem XP
bool g_hasSoulFeeder = false; // soul feeding is twice as potent

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
    for (const auto& def : meo::kGems) {
        ResolvedGem rg;
        rg.def = &def;
        rg.mgef = dh->LookupForm<RE::EffectSetting>(def.mgefID, def.plugin);
        if (!rg.mgef) {
            spdlog::warn("gem '{}' disabled: MGEF {:06X} not found in {}", def.gid, def.mgefID, def.plugin);
        }
        const int levels = def.singleLevel ? 1 : 5;
        RE::TESObjectMISC* prev = nullptr;
        for (int lv = 0; lv < levels; ++lv) {
            rg.items[lv] = dh->LookupForm<RE::TESObjectMISC>(def.gemItem[lv], kPluginName);
            // Short-curve gems (e.g. Muffle) pad higher levels with the top
            // form; map each distinct form to its FIRST level only.
            if (rg.items[lv] && rg.mgef && rg.items[lv] != prev) {
                g_gemByItem[rg.items[lv]->GetFormID()] = { static_cast<int>(g_gems.size()), lv + 1 };
            }
            prev = rg.items[lv];
        }
        g_gemByGid[def.gid] = static_cast<int>(g_gems.size());
        if (rg.mgef) {
            ++ok;
        }
        g_gems.push_back(rg);
    }
    // Weighted spawn pools: each gem is pushed spawnWeight times so a uniform
    // pick becomes a tier rarity curve (S rarest — see SPAWN_WEIGHT). Corpse
    // drops pull weapon+armor; world-weapon stamps pull weapon-domain only.
    for (std::size_t i = 0; i < g_gems.size(); ++i) {
        if (g_gems[i].mgef && !g_gems[i].def->singleLevel) {
            for (int w = 0, n = std::max<int>(g_gems[i].def->spawnWeight, 1); w < n; ++w) {
                g_corpseGems.push_back(static_cast<int>(i));
                if (!g_gems[i].def->isArmor) {
                    g_lootableGems.push_back(static_cast<int>(i));
                }
            }
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
    for (int i = 0; i < 5; ++i) {
        g_filledSoulGems[i] = dh->LookupForm<RE::TESSoulGem>(kFilledSoulGemIDs[i], "Skyrim.esm");
        g_perkAttune[i] = dh->LookupForm<RE::BGSPerk>(kPerkAttuneBase + i, kPluginName);
    }
    g_perkGemCutter = dh->LookupForm<RE::BGSPerk>(kPerkGemCutter, kPluginName);
    g_perkSoulFeeder = dh->LookupForm<RE::BGSPerk>(kPerkSoulFeeder, kPluginName);
    spdlog::info("catalog resolved: {}/{} gems live (weapon+armor), {} socketable gem items, pouch={}, "
                 "mentor={}, soulCairn={}, bossType={}",
                 ok, std::size(meo::kGems), g_gemByItem.size(),
                 g_pouchSpell ? "ok" : "MISSING", g_mentorGem ? "ok" : "MISSING",
                 g_soulCairn ? "ok" : "absent", g_bossRefType ? "ok" : "MISSING");
}

// Sockets an item can hold (m13 multi-socket). Boots are excluded upstream
// (ineligible). Gloves are dual by default; chest/weapon 2nd sockets arrive
// with the socket perks (3b-2). Everything else is single.
int SocketCapacity(RE::TESBoundObject* a_obj) {
    if (auto* armo = a_obj ? a_obj->As<RE::TESObjectARMO>() : nullptr) {
        if (armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kHands)) {
            return 2;  // gloves
        }
        return 1;
    }
    return 1;  // weapons
}

// Rebuild an instance's SINGLE combined enchantment from ALL its filled socket
// slots. One created enchant carries one Effect per gem; the name lists them.
// No filled slots -> strip the enchant + name. Caller applies the worn ability.
// This is the multi-socket core: every socket/unsocket/level-up rebuilds here.
void RebuildInstanceEnchant(RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList) {
    auto* xid = a_xList ? a_xList->GetByType<RE::ExtraUniqueID>() : nullptr;
    if (!a_base || !xid) {
        return;
    }
    const std::uint16_t uid = xid->uniqueID;
    const bool          isArmor = a_base->Is(RE::FormType::Armor);

    struct Filled { const ResolvedGem* rg; int lvIdx; };
    std::vector<Filled> filled;
    for (int slot = 0; slot < kMaxSockets; ++slot) {
        auto it = g_sockets.find(MakeKey(a_base->GetFormID(), uid, static_cast<std::uint8_t>(slot)));
        if (it == g_sockets.end()) {
            continue;
        }
        auto gemIt = g_gemByGid.find(it->second.gid);
        if (gemIt == g_gemByGid.end() || !g_gems[gemIt->second].mgef) {
            continue;
        }
        filled.push_back({ &g_gems[gemIt->second], std::clamp<int>(it->second.level, 1, 5) - 1 });
    }
    if (filled.empty()) {  // last gem removed — return the item to plain
        a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
        a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);
        return;
    }

    RE::BSTArray<RE::Effect> effects;
    effects.resize(filled.size());
    std::string namePart;
    for (std::size_t i = 0; i < filled.size(); ++i) {
        auto& eff = effects[i];
        // Master power scale (MCM) × Gem Attunement (+8%/rank, DESIGN §6).
        eff.effectItem.magnitude =
            filled[i].rg->def->magnitude[filled[i].lvIdx] * g_magnitudeMult *
            (1.0f + 0.08f * g_attuneRank);
        eff.effectItem.area = 0;
        eff.effectItem.duration = static_cast<std::uint32_t>(filled[i].rg->def->duration);
        eff.baseEffect = filled[i].rg->mgef;
        eff.cost = 0.0f;
        if (!namePart.empty()) {
            namePart += " + ";
        }
        namePart += std::format("{} {}", filled[i].rg->def->name, meo::kRoman[filled[i].lvIdx]);
    }
    auto* com = RE::BGSCreatedObjectManager::GetSingleton();
    auto* ench = isArmor ? com->AddArmorEnchantment(effects) : com->AddWeaponEnchantment(effects);
    if (!ench) {
        spdlog::error("[rebuild] Add{}Enchantment null on {:08X}/{}",
                      isArmor ? "Armor" : "Weapon", a_base->GetFormID(), uid);
        return;
    }
    ench->data.costOverride = 0;  // gems never drain (ENGINE_NOTES §3/§8)
    ench->data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);
    if (auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>()) {
        xEnch->enchantment = ench;
        xEnch->charge = 0xFFFF;
        xEnch->removeOnUnequip = false;
    } else {
        a_xList->Add(new RE::ExtraEnchantment(ench, 0xFFFF, false));
    }
    // Forced name + engine reconcile (M2d/M2h; no brackets M2i).
    const char*       baseName = a_base->GetName();
    const std::string newName = std::format("{} {}", namePart,
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
    xText->GetDisplayName(a_base, health);
    spdlog::info("[rebuild] {:08X}/{}: '{}' ({} gem(s))", a_base->GetFormID(), uid, newName,
                 filled.size());
}

// Socket one gem into slot a_slot, then rebuild the instance's combined enchant.
// Returns the uid (minted if absent), or 0 on failure. Caller does the worn
// ability + gem consumption. a_xp carries banked XP through a level-up re-stamp.
std::uint16_t StampInstance(RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                            int a_gemIdx, int a_level, std::uint8_t a_slot = 0, float a_xp = 0.0f) {
    if (a_gemIdx < 0 || !a_base || !a_xList || !g_gems[a_gemIdx].mgef) {
        return 0;
    }
    std::uint16_t uid = 0;
    if (auto* xid = a_xList->GetByType<RE::ExtraUniqueID>()) {
        uid = xid->uniqueID;  // engine-assigned is fine: key includes baseID
    } else {
        uid = g_nextUID++;
        a_xList->Add(new RE::ExtraUniqueID(a_base->GetFormID(), uid));
    }
    const int lvIdx = std::clamp(a_level, 1, 5) - 1;
    g_sockets[MakeKey(a_base->GetFormID(), uid, a_slot)] =
        SocketRecord{ g_gems[a_gemIdx].def->gid, static_cast<std::uint8_t>(lvIdx + 1), a_xp };
    RebuildInstanceEnchant(a_base, a_xList);
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

// Weapon-base eligibility, shared by the gem menu and world-loot rolls.
// All engine-owned verdicts: GetPlayable() rejects non-playable WEAPs
// (Dawnguard's Aetherium Crest is one), IsHandToHandMelee() rejects
// unarmed pseudo-weapons, IsBound() rejects conjured weapons, and
// MagicDisallowEnchanting is the enchanting table's own artifact rule —
// if the table refuses the item, a gem does too.
bool IsSocketableWeaponBase(const RE::TESObjectWEAP* a_weap) {
    return a_weap && !a_weap->formEnchanting && a_weap->GetPlayable() &&
           !a_weap->IsHandToHandMelee() && !a_weap->IsBound() &&
           !a_weap->HasKeywordString("MagicDisallowEnchanting");
}

// Armor-base eligibility (M8b armor gems). Socketable slots per DESIGN §4:
// head, body, hands, amulet, ring — boots (kFeet) get no socket. Same
// engine verdicts as weapons (playable, not base-enchanted, table allows it).
bool IsSocketableArmorBase(const RE::TESObjectARMO* a_armo) {
    if (!a_armo || a_armo->formEnchanting || !a_armo->GetPlayable() ||
        a_armo->HasKeywordString("MagicDisallowEnchanting")) {
        return false;
    }
    // NB: HasPartOf() is .all() (every bit must match), so it must be called
    // per-slot and OR'd — a combined mask would demand one piece fill all slots.
    using S = RE::BGSBipedObjectForm::BipedObjectSlot;
    return a_armo->HasPartOf(S::kHead) || a_armo->HasPartOf(S::kBody) ||
           a_armo->HasPartOf(S::kHands) || a_armo->HasPartOf(S::kAmulet) ||
           a_armo->HasPartOf(S::kRing);
}

// Re-apply a socketed instance's ability to an already-worn item after a
// (re-)stamp. Weapons take a hand; worn armor uses the slotless call.
void ApplyWornAbility(RE::Actor* a_owner, RE::TESBoundObject* a_base,
                      RE::ExtraDataList* a_xList, bool a_left) {
    if (!a_owner || !a_base) {
        return;
    }
    if (a_base->Is(RE::FormType::Armor)) {
        a_owner->UpdateArmorAbility(a_base, a_xList);
    } else {
        a_owner->UpdateWeaponAbility(a_base, a_xList, a_left);
    }
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
float g_gemDropChance = 0.03f;    // [Loot] fGemDropChance — corpse gem on player kill
float g_worldSocketChance = 0.05f;// [Loot] fWorldSocketChance — world weapon born socketed
float g_gemLevel2Chance = 0.02f;  // [Loot] fGemLevel2Chance — spawned gem is level II not I
float g_bossXPMult = 10.0f;       // [XP] fBossXPMult — boss/dragon kill multiplier
bool  g_xpNotify = true;          // [UI] bXPNotify — "Gem XP +N" on kills
// g_magnitudeMult [XP] fMagnitudeMult is declared up top (StampInstance uses it)

// Apply one INI file's key=value lines onto the config globals. Tolerates the
// MCM Helper format: a leading UTF-8 BOM and [Section] headers (no '=' → skipped;
// keys are globally unique so sections are informational only).
static void ApplyIniFile(const char* a_path) {
    std::ifstream ini(a_path);
    if (!ini) {
        return;
    }
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    };
    std::string line;
    while (std::getline(ini, line)) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3);  // UTF-8 BOM (MCM Helper writes one)
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const float       val = std::strtof(trim(line.substr(eq + 1)).c_str(), nullptr);
        if (key == "fXPPerKill")              g_xpPerKill = val;
        else if (key == "fGemDropChance")     g_gemDropChance = val;
        else if (key == "fWorldSocketChance") g_worldSocketChance = val;
        else if (key == "fGemLevel2Chance")   g_gemLevel2Chance = val;
        else if (key == "fBossXPMult")        g_bossXPMult = val;
        else if (key == "fMagnitudeMult")     g_magnitudeMult = val;
        else if (key == "bXPNotify")          g_xpNotify = val != 0.0f;
    }
}

// Legacy SKSE/Plugins/MEO.ini is a dev/seed file; the MCM's own settings file
// (written by MCM Helper) is read last so it wins. Called at load and re-called
// live on menu close (ReloadConfigIfChanged) so MCM edits apply immediately.
void ReadConfig() {
    ApplyIniFile("Data/SKSE/Plugins/MEO.ini");
    ApplyIniFile("Data/MCM/Settings/MEO.ini");
    if (g_xpPerKill != 1.0f) {
        spdlog::warn("DEV: fXPPerKill={} override", g_xpPerKill);
    }
    spdlog::info("config: drop={:.3f} world={:.3f} lvl2={:.3f} xp={:.2f} boss={:.1f} mag={:.2f} notify={}",
                 g_gemDropChance, g_worldSocketChance, g_gemLevel2Chance, g_xpPerKill,
                 g_bossXPMult, g_magnitudeMult, g_xpNotify);
}

// DESIGN §6 perks. Until the C# installer replaces the load order's enchanting
// tree with MEO's, auto-grant MEO perks by the player's Enchanting skill
// (Attunement 1..5 at 0/20/40/60/80, Gem Cutter at 20, Soul Feeder at 40),
// then cache what the player actually holds. Effects live in StampInstance
// (magnitude), AwardKillXP (Gem Cutter) and FeedSoulToGem (Soul Feeder).
void RefreshPerks() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* avo = player ? player->AsActorValueOwner() : nullptr;
    if (!player || !avo) {
        return;
    }
    const float skill = avo->GetBaseActorValue(RE::ActorValue::kEnchanting);
    auto grant = [&](RE::BGSPerk* p, bool want) {
        if (p && want && !player->HasPerk(p)) {
            player->AddPerk(p, 1);
        }
    };
    const int rank = 1 + (skill >= 20) + (skill >= 40) + (skill >= 60) + (skill >= 80);
    for (int i = 0; i < 5; ++i) {
        grant(g_perkAttune[i], i < rank);
    }
    grant(g_perkGemCutter, skill >= 20.0f);
    grant(g_perkSoulFeeder, skill >= 40.0f);
    g_attuneRank = 0;
    for (int i = 0; i < 5; ++i) {
        if (g_perkAttune[i] && player->HasPerk(g_perkAttune[i])) {
            ++g_attuneRank;
        }
    }
    g_hasGemCutter = g_perkGemCutter && player->HasPerk(g_perkGemCutter);
    g_hasSoulFeeder = g_perkSoulFeeder && player->HasPerk(g_perkSoulFeeder);
    spdlog::info("[perks] enchanting={:.0f} attuneRank={} gemCutter={} soulFeeder={}",
                 skill, g_attuneRank, g_hasGemCutter, g_hasSoulFeeder);
}

// Menu snapshot rows + shared state (declared here so MenuSink can read
// g_menu.station; the menu is built/drawn much further down).
struct MenuItemRow {
    std::string   label;
    RE::FormID    base = 0;
    std::uint16_t uid = 0;       // 0 = plain stack (xList materialized on socket)
    bool          worn = false;
    bool          isArmor = false;    // gates which gems can socket into it
    int           capacity = 1;       // socket slots this item has (1 or 2)
    std::string   slotGem[2];         // per-slot gem label; empty = empty slot
};
struct MenuGemRow {
    std::string   label;
    RE::FormID    base = 0;
    std::uint16_t uid = 0;       // instance with banked XP, else 0
    bool          isArmor = false;
};
struct MenuState {
    std::mutex               lock;
    std::atomic<bool>        open{ false };
    std::atomic<bool>        busy{ false };      // an action task is in flight
    std::atomic<bool>        wantClose{ false }; // set by input, consumed by draw
    std::atomic<bool>        station{ false };   // opened at an enchanting station (feed/destroy)
    std::vector<MenuItemRow> items;
    std::vector<MenuGemRow>  gems;
    int                      selItem = 0;
};
MenuState g_menu;

void OpenGemMenu(bool a_station = false);  // defined with the render hooks below
void CloseGemMenu();

// Live MCM apply: SkyUI's Mod Configuration menu is hosted in the Journal
// (pause) menu, and MCM Helper persists ModSettings to MEO.ini when it closes.
// Re-reading the INI on that close makes slider/toggle changes take effect
// immediately. Also: opening an ENCHANTING station's Crafting Menu opens the
// gem menu in station mode (soul feeding / destruction, DESIGN §3).
class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuSink* GetSingleton() {
        static MenuSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (!a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
            ReadConfig();
            RefreshPerks();  // pick up Enchanting skill-ups (interim auto-grant)
        } else if (!a_event->opening && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
            if (g_menu.station.load()) {
                SKSE::GetTaskInterface()->AddTask([]() { CloseGemMenu(); });
            }
        } else if (a_event->opening && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
            SKSE::GetTaskInterface()->AddTask([]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto  furn = player ? player->GetOccupiedFurniture() : RE::ObjectRefHandle{};
                auto  ref = furn.get();
                auto* base = ref ? ref->GetBaseObject() : nullptr;
                auto* f = base ? base->As<RE::TESFurniture>() : nullptr;
                using BT = RE::TESFurniture::WorkBenchData::BenchType;
                if (f && f->workBenchData.benchType == BT::kEnchanting) {
                    OpenGemMenu(true);
                }
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

bool IsWornXList(const RE::ExtraDataList* a_xl);  // defined below

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
    const int newLevel = a_rec.level + 1;
    // a_rec is the live g_sockets slot entry; bump its level (xp stays cumulative)
    // and rebuild the instance's combined enchant from all slots.
    a_rec.level = static_cast<std::uint8_t>(newLevel);
    RebuildInstanceEnchant(a_base, a_xList);
    if (IsWornXList(a_xList)) {  // non-worn (fed at a station) re-applies on equip
        ApplyWornAbility(a_owner, a_base, a_xList, a_left);
    }
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
    // DESIGN §6: the carrier's Enchanting skill (incl. Fortify Enchanting from
    // gear/potions) scales Gem XP gain — x(1 + AV/100). Enchanting now grows a
    // gem, so investing in it pays off directly.
    if (auto* avo = a_owner->AsActorValueOwner()) {
        xp *= 1.0f + std::max(0.0f, avo->GetActorValue(RE::ActorValue::kEnchanting)) / 100.0f;
    }
    if (g_hasGemCutter && a_owner->IsPlayerRef()) {  // Gem Cutter: +50% Gem XP
        xp *= 1.5f;
    }
    int awarded = 0;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists ||
            !(entry->object->Is(RE::FormType::Weapon) || entry->object->Is(RE::FormType::Armor))) {
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
            // Feed every filled socket slot on this worn item.
            for (int slot = 0; slot < kMaxSockets; ++slot) {
                auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                                 static_cast<std::uint8_t>(slot)));
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
    }
    // "Gem XP +N" HUD feedback (player only; bXPNotify, MCM toggle later).
    if (awarded > 0 && g_xpNotify && a_owner->IsPlayerRef()) {
        Notify(awarded == 1 ? std::format("Gem XP +{:.0f}", xp)
                            : std::format("Gem XP +{:.0f} (x{} gems)", xp, awarded));
    }
}

// ── M6: the native ImGui gem menu ─────────────────────────────────────
// Render pattern (verified from D7ry/wheeler source, NG 3.7 API): a
// write_call<5> thunk after renderer init grabs device/context/swapchain
// from RE::BSGraphics::Renderer and initializes the ImGui win32+dx11
// backends; a thunk on the DXGI-present call renders our frame. Input is a
// thunk on the input-dispatch call: while the menu is open every event is
// translated for ImGui and swallowed (the game sees nothing). All engine
// mutation is deferred to SKSE tasks (main thread); the draw thread only
// reads a mutex-guarded snapshot.

// Live ExtraDataList of instance (a_form, a_uid) in a_owner's inventory
// (the proven M4b re-find).
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

// Hand the player a gem at the given level carrying its banked XP (M4b
// recipe: spawn a real reference, stamp its engine-owned extraList, pick
// it up — extras survive player pickup; proven).
void GiveGemInstance(int a_gemIdx, int a_level, float a_xp) {
    auto*       player = RE::PlayerCharacter::GetSingleton();
    const auto& rg = g_gems[a_gemIdx];
    const int   lvIdx = std::clamp(a_level, 1, 5) - 1;
    auto*       gemForm = rg.items[lvIdx];
    if (!player || !gemForm) {
        return;
    }
    if (a_xp <= 0.0f || a_level >= 5) {
        player->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
        return;
    }
    auto ref = player->PlaceObjectAtMe(gemForm, false);
    if (!ref) {
        spdlog::error("[menu] PlaceObjectAtMe failed for '{}' — plain gem given instead", rg.def->gid);
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
    xText->GetDisplayName(gemForm, 1.0f);
    g_sockets[MakeKey(gemForm->GetFormID(), uid)] =
        SocketRecord{ rg.def->gid, static_cast<std::uint8_t>(lvIdx + 1), a_xp };
    player->PickUpObject(ref.get(), 1, false, false);
    spdlog::info("[menu] gem instance {:08X}/{}: '{}' as '{}'", gemForm->GetFormID(), uid,
                 rg.def->gid, name);
}

// ── Menu state: draw thread reads, SKSE tasks write ───────────────────

bool IsWornXList(const RE::ExtraDataList* a_xl) {
    return a_xl->HasType(RE::ExtraDataType::kWorn) ||
           a_xl->HasType(RE::ExtraDataType::kWornLeft);
}

// Main thread only. Rebuilds both panes from the live inventory. Weapon
// instances without a uid get one stamped eagerly (Wheeler does the same
// for all weapons/armor) so every listed row has a stable identity.
void BuildMenuSnapshot() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    std::vector<MenuItemRow> items;
    std::vector<MenuGemRow>  gems;
    auto inv = player->GetInventory([](RE::TESBoundObject& o) {
        return o.Is(RE::FormType::Weapon) || o.Is(RE::FormType::Armor) ||
               o.Is(RE::FormType::Misc);
    });
    for (const auto& [obj, data] : inv) {
        if (data.first <= 0) {
            continue;
        }
        const bool isWeap = obj->Is(RE::FormType::Weapon);
        const bool isArmor = obj->Is(RE::FormType::Armor);
        if (isWeap || isArmor) {
            // Ineligible bases are hidden — but an instance that already
            // holds one of our gems stays listed so the gem can come back.
            const bool eligible = isWeap
                ? IsSocketableWeaponBase(obj->As<RE::TESObjectWEAP>())
                : IsSocketableArmorBase(obj->As<RE::TESObjectARMO>());
            const char* baseName = obj->GetName();
            if (!baseName || !*baseName) {
                continue;
            }
            std::int32_t instances = 0;
            if (data.second && data.second->extraLists) {
                for (auto* xl : *data.second->extraLists) {
                    if (!xl) {
                        continue;
                    }
                    const std::int32_t cnt = std::max(xl->GetCount(), 1);
                    auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                    MenuItemRow row;
                    row.base = obj->GetFormID();
                    row.isArmor = isArmor;
                    row.worn = IsWornXList(xl);
                    if (xl->HasType(RE::ExtraDataType::kEnchantment)) {
                        // Enchanted units are distinct — never part of the plain pool.
                        instances += cnt;
                        if (!xid) {
                            continue;  // enchanted, never stamped by us
                        }
                        // Ours only if at least one socket slot has a record;
                        // otherwise it's a player/base enchant — not socketable.
                        bool ours = false;
                        for (int s = 0; s < kMaxSockets; ++s) {
                            if (g_sockets.contains(MakeKey(row.base, xid->uniqueID,
                                                           static_cast<std::uint8_t>(s)))) {
                                ours = true;
                                break;
                            }
                        }
                        if (!ours) {
                            continue;
                        }
                    } else {
                        if (!eligible) {
                            continue;
                        }
                        // A stack sharing one extraList (count > 1) must NOT become
                        // a single instance row: stamping it would enchant every unit
                        // for one gem. Leave those in the plain pool (below).
                        if (cnt > 1) {
                            continue;
                        }
                        instances += cnt;  // a genuine singleton instance
                        if (!xid) {
                            xid = new RE::ExtraUniqueID(obj->GetFormID(), g_nextUID++);
                            xl->Add(xid);
                        }
                    }
                    row.uid = xid->uniqueID;
                    row.capacity = SocketCapacity(obj);
                    for (int s = 0; s < row.capacity && s < kMaxSockets; ++s) {
                        auto it = g_sockets.find(MakeKey(row.base, xid->uniqueID,
                                                         static_cast<std::uint8_t>(s)));
                        if (it == g_sockets.end()) {
                            continue;
                        }
                        if (auto gemIt = g_gemByGid.find(it->second.gid); gemIt != g_gemByGid.end()) {
                            const auto& rg = g_gems[gemIt->second];
                            const float need = NextThreshold(rg.def, it->second.level);
                            row.slotGem[s] = std::format("{} {}", rg.def->name,
                                                         meo::kRoman[it->second.level - 1]);
                            if (need > 0.0f) {
                                row.slotGem[s] += std::format(" ({:.0f}/{:.0f})", it->second.xp, need);
                            }
                        } else {
                            row.slotGem[s] = "gem from a missing master";
                        }
                    }
                    row.label = baseName;
                    if (row.worn) {
                        row.label += "  (worn)";
                    }
                    items.push_back(std::move(row));
                }
            }
            if (eligible && data.first - instances > 0) {
                MenuItemRow row;
                row.base = obj->GetFormID();
                row.isArmor = isArmor;
                row.capacity = SocketCapacity(obj);
                row.label = baseName;
                if (data.first - instances > 1) {
                    row.label += std::format("  x{}", data.first - instances);
                }
                items.push_back(std::move(row));
            }
        } else {
            auto it = g_gemByItem.find(obj->GetFormID());
            if (it == g_gemByItem.end()) {
                continue;
            }
            std::int32_t plain = data.first;
            if (data.second && data.second->extraLists) {
                for (auto* xl : *data.second->extraLists) {
                    if (!xl) {
                        continue;
                    }
                    plain -= std::max(xl->GetCount(), 1);
                    auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                    auto  recIt = xid ? g_sockets.find(MakeKey(obj->GetFormID(), xid->uniqueID))
                                      : g_sockets.end();
                    if (recIt == g_sockets.end()) {
                        ++plain;  // no banked record: behaves as a plain gem
                        continue;
                    }
                    const auto& rg = g_gems[it->second.first];
                    MenuGemRow  row;
                    row.base = obj->GetFormID();
                    row.isArmor = rg.def->isArmor;
                    row.uid = xid->uniqueID;
                    row.label = std::format("{} ({:.0f}/{:.0f})", obj->GetName(), recIt->second.xp,
                                            NextThreshold(rg.def, recIt->second.level));
                    gems.push_back(std::move(row));
                }
            }
            if (plain > 0) {
                MenuGemRow row;
                row.base = obj->GetFormID();
                row.isArmor = g_gems[it->second.first].def->isArmor;
                row.label = obj->GetName();
                if (plain > 1) {
                    row.label += std::format("  x{}", plain);
                }
                gems.push_back(std::move(row));
            }
        }
    }
    std::sort(items.begin(), items.end(), [](const MenuItemRow& a, const MenuItemRow& b) {
        if (a.worn != b.worn) {
            return a.worn;
        }
        return a.label < b.label;
    });
    std::sort(gems.begin(), gems.end(), [](const MenuGemRow& a, const MenuGemRow& b) {
        return a.label < b.label;
    });
    std::scoped_lock lk(g_menu.lock);
    g_menu.items = std::move(items);
    g_menu.gems = std::move(gems);
    g_menu.selItem = std::clamp(g_menu.selItem, 0,
                                std::max(0, static_cast<int>(g_menu.items.size()) - 1));
}

// ── Menu actions (SKSE tasks — main thread; all M4b-proven flows) ─────
void MenuUnsocket(RE::FormID a_base, std::uint16_t a_uid, std::uint8_t a_slot) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* form = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
    auto  it = g_sockets.find(MakeKey(a_base, a_uid, a_slot));
    auto* xl = form ? FindInstanceXList(player, form, a_uid) : nullptr;
    if (!player || it == g_sockets.end() || !xl) {
        spdlog::warn("[menu] unsocket failed: {:08X}/{}[{}] rec={} xl={}", a_base, a_uid, a_slot,
                     it != g_sockets.end(), xl != nullptr);
        return;
    }
    const SocketRecord rec = it->second;
    auto gemIt = g_gemByGid.find(rec.gid);
    if (gemIt == g_gemByGid.end()) {
        Notify("That gem's essence is missing from this load order.");
        return;
    }
    g_sockets.erase(it);
    // Rebuild from whatever slots remain (strips the enchant + name if none).
    RebuildInstanceEnchant(form, xl);
    if (IsWornXList(xl)) {
        ApplyWornAbility(player, form, xl, xl->HasType(RE::ExtraDataType::kWornLeft));
    }
    spdlog::info("[menu] unsocketed {:08X}/{}[{}]: '{}' L{} xp={:.0f}", a_base, a_uid, a_slot,
                 rec.gid, rec.level, rec.xp);
    GiveGemInstance(gemIt->second, rec.level, rec.xp);
    const auto& rg = g_gems[gemIt->second];
    Notify(std::format("{} {} returned to your pouch.", rg.def->name, meo::kRoman[rec.level - 1]));
}

// M10 (stage 2b): destroy a socketed gem at a station, reclaiming 1/10 of its
// banked Gem XP as the largest filled soul gem it affords (the only investment
// sink that recovers anything). The gem itself is gone — not returned.
void DestroyGem(RE::FormID a_base, std::uint16_t a_uid, std::uint8_t a_slot) {
    static const char* kSoulNames[5] = { "petty", "lesser", "common", "greater", "grand" };
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* form = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
    auto  it = g_sockets.find(MakeKey(a_base, a_uid, a_slot));
    auto* xl = form ? FindInstanceXList(player, form, a_uid) : nullptr;
    if (!player || it == g_sockets.end() || !xl) {
        return;
    }
    const SocketRecord rec = it->second;
    const float        reclaim = rec.xp / 10.0f;
    int                tier = -1;
    for (int i = 0; i < 5; ++i) {
        if (reclaim >= kSoulFeedXP[i]) {
            tier = i;
        }
    }
    g_sockets.erase(it);
    RebuildInstanceEnchant(form, xl);  // rebuild from remaining slots (or strip)
    if (IsWornXList(xl)) {
        ApplyWornAbility(player, form, xl, xl->HasType(RE::ExtraDataType::kWornLeft));
    }
    if (tier >= 0 && g_filledSoulGems[tier]) {
        player->AddObjectToContainer(g_filledSoulGems[tier], nullptr, 1, nullptr);
        Notify(std::format("Gem destroyed — its essence yields a {} soul gem.", kSoulNames[tier]));
    } else {
        Notify("Gem destroyed — too little essence to reclaim a soul.");
    }
    spdlog::info("[destroy] {:08X}/{} '{}' L{} xp={:.0f} -> soul tier {}", a_base, a_uid, rec.gid,
                 rec.level, rec.xp, tier);
}

void MenuSocket(RE::FormID a_itemBase, std::uint16_t a_itemUid, RE::FormID a_gemBase,
                std::uint16_t a_gemUid) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* itemForm = RE::TESForm::LookupByID<RE::TESBoundObject>(a_itemBase);
    auto* gemForm = RE::TESForm::LookupByID<RE::TESObjectMISC>(a_gemBase);
    auto  gemMapIt = g_gemByItem.find(a_gemBase);
    if (!player || !itemForm || !gemForm || gemMapIt == g_gemByItem.end()) {
        return;
    }
    // Weapon gems only fit weapons; armor gems only fit armor.
    if (g_gems[gemMapIt->second.first].def->isArmor != itemForm->Is(RE::FormType::Armor)) {
        Notify("That gem doesn't fit that kind of gear.");
        return;
    }
    RE::ExtraDataList* xl = nullptr;
    if (a_itemUid) {
        xl = FindInstanceXList(player, itemForm, a_itemUid);
    } else {
        // A never-touched plain stack: run one unit through the proven
        // drop-stamp-pickup flow so the ENGINE mints its extra list (NG 3.7
        // declares but does not export the ExtraDataList constructor, and
        // hand-building one would break the engine-flows rule anyway).
        const auto dropped = player->RemoveItem(itemForm, 1, RE::ITEM_REMOVE_REASON::kDropping,
                                                nullptr, nullptr);
        if (auto ref = dropped.get()) {
            const std::uint16_t uid = g_nextUID++;
            ref->extraList.Add(new RE::ExtraUniqueID(a_itemBase, uid));
            player->PickUpObject(ref.get(), 1, false, false);
            xl = FindInstanceXList(player, itemForm, uid);
            spdlog::info("[menu] minted instance {:08X}/{} via drop/pickup", a_itemBase, uid);
        }
    }
    if (!xl) {
        Notify("That item is no longer in your inventory.");
        return;
    }
    // Multi-socket: fill the next free slot (up to this item's capacity). Our
    // own combined enchant doesn't block — only a foreign (player/base) one.
    auto* itemXid = xl->GetByType<RE::ExtraUniqueID>();
    const std::uint16_t uid = itemXid ? itemXid->uniqueID : 0;
    const int cap = SocketCapacity(itemForm);
    int  freeSlot = -1;
    bool ourSockets = false;
    for (int s = 0; s < cap; ++s) {
        if (g_sockets.contains(MakeKey(a_itemBase, uid, static_cast<std::uint8_t>(s)))) {
            ourSockets = true;
        } else if (freeSlot < 0) {
            freeSlot = s;
        }
    }
    if (xl->HasType(RE::ExtraDataType::kEnchantment) && !ourSockets) {
        Notify("That item is already enchanted.");
        return;
    }
    if (freeSlot < 0) {
        Notify(cap > 1 ? "Every socket is already filled." : "That item's socket is filled.");
        return;
    }
    const auto [gemIdx, itemLevel] = gemMapIt->second;
    int          level = itemLevel;
    float        xp = 0.0f;
    bool         hadRec = false;
    SocketRecord saved{};
    RE::ExtraDataList* gemXL = nullptr;
    if (a_gemUid) {
        gemXL = FindInstanceXList(player, gemForm, a_gemUid);
        if (auto it = g_sockets.find(MakeKey(a_gemBase, a_gemUid)); it != g_sockets.end()) {
            saved = it->second;
            hadRec = true;
            level = saved.level;
            xp = saved.xp;
            g_sockets.erase(it);
        }
        if (!gemXL) {
            Notify("That gem is no longer in your inventory.");
            if (hadRec) {
                g_sockets[MakeKey(a_gemBase, a_gemUid)] = saved;
            }
            return;
        }
    } else {
        const auto counts = player->GetInventoryCounts(
            [&](RE::TESBoundObject& o) { return &o == gemForm; });
        if (counts.empty()) {
            Notify("That gem is no longer in your inventory.");
            return;
        }
    }
    if (!StampInstance(itemForm, xl, gemIdx, level, static_cast<std::uint8_t>(freeSlot), xp)) {
        if (hadRec) {
            g_sockets[MakeKey(a_gemBase, a_gemUid)] = saved;
        }
        return;
    }
    if (IsWornXList(xl)) {
        ApplyWornAbility(player, itemForm, xl, xl->HasType(RE::ExtraDataType::kWornLeft));
    }
    player->RemoveItem(gemForm, 1, RE::ITEM_REMOVE_REASON::kRemove, gemXL, nullptr);
    const auto& rg = g_gems[gemIdx];
    Notify(std::format("{} {} socketed into {}.", rg.def->name, meo::kRoman[level - 1],
                       itemForm->GetName()));
}

// M10 (stage 2a): consume the smallest filled, non-reusable soul gem and
// grant its Gem XP (DESIGN §3) to one socketed gem. Enchanting-station only.
void FeedSoulToGem(RE::FormID a_itemBase, std::uint16_t a_uid, std::uint8_t a_slot) {
    static const char* kSoulNames[5] = { "petty", "lesser", "common", "greater", "grand" };
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* itemForm = RE::TESForm::LookupByID<RE::TESBoundObject>(a_itemBase);
    if (!player || !itemForm) {
        return;
    }
    auto recIt = g_sockets.find(MakeKey(a_itemBase, a_uid, a_slot));
    if (recIt == g_sockets.end()) {
        Notify("That gem is no longer socketed.");
        return;
    }
    auto* xl = FindInstanceXList(player, itemForm, a_uid);
    if (!xl) {
        Notify("That item is no longer in your inventory.");
        return;
    }
    // Smallest filled soul gem wins (fuel efficiency); Azura's Star etc. skipped.
    auto* changes = player->GetInventoryChanges();
    RE::TESSoulGem*    bestGem = nullptr;
    RE::ExtraDataList* bestXL = nullptr;
    int                bestSoul = 99;
    auto consider = [&](RE::TESSoulGem* sg, RE::ExtraDataList* xs, int soul) {
        if (soul >= 1 && soul < bestSoul) {
            bestSoul = soul; bestGem = sg; bestXL = xs;
        }
    };
    if (changes && changes->entryList) {
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object) {
                continue;
            }
            auto* sg = entry->object->As<RE::TESSoulGem>();
            if (!sg || (g_reusableSoulGemKW && sg->HasKeyword(g_reusableSoulGemKW))) {
                continue;
            }
            const int baseSoul = static_cast<int>(sg->GetContainedSoul());
            bool hadInstance = false;
            if (entry->extraLists) {
                for (auto* xs : *entry->extraLists) {
                    if (!xs) {
                        continue;
                    }
                    hadInstance = true;
                    int s = baseSoul;
                    if (auto* es = xs->GetByType<RE::ExtraSoul>()) {
                        s = static_cast<int>(es->GetContainedSoul());
                    }
                    consider(sg, xs, s);
                }
            }
            if (!hadInstance) {
                consider(sg, nullptr, baseSoul);
            }
        }
    }
    if (!bestGem || bestSoul < 1) {
        Notify("You have no filled soul gems to feed.");
        return;
    }
    const int   idx = std::clamp(bestSoul, 1, 5) - 1;
    const float xp = kSoulFeedXP[idx] * (g_hasSoulFeeder ? 2.0f : 1.0f);  // Soul Feeder: x2
    player->RemoveItem(bestGem, 1, RE::ITEM_REMOVE_REASON::kRemove, bestXL, nullptr);
    auto gemIt = g_gemByGid.find(recIt->second.gid);
    if (gemIt == g_gemByGid.end()) {
        return;
    }
    const bool left = xl->HasType(RE::ExtraDataType::kWornLeft);
    const bool grew = GrantGemXP(player, itemForm, xl, left, recIt->second, gemIt->second, xp, a_uid);
    Notify(grew ? std::format("Fed a {} soul (+{:.0f} Gem XP).", kSoulNames[idx], xp)
                : "That gem is already mastered — the soul is spent for nothing.");
    spdlog::info("[feed] {} soul -> {}/{} : +{:.0f} xp", kSoulNames[idx], a_itemBase, a_uid, xp);
}

void QueueMenuTask(std::function<void()> a_fn) {
    g_menu.busy = true;
    SKSE::GetTaskInterface()->AddTask([fn = std::move(a_fn)]() {
        fn();
        BuildMenuSnapshot();
        g_menu.busy = false;
    });
}

void CloseGemMenu() {
    g_menu.open = false;
    g_menu.wantClose = false;
}

void OpenGemMenu(bool a_station);  // declared earlier; defined after the render hooks

// ── Render + input hooks (Wheeler pattern, IDs verified from source) ──
namespace menuhook {

    ID3D11Device*        g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    std::atomic<bool>    g_d3dReady{ false };
    // Backbuffer size, cached at init. The Win32 backend derives
    // io.DisplaySize from GetClientRect, which under Proton/upscalers can
    // disagree with the render target — ImGui then draws off-center. The
    // present hook overrides DisplaySize with these every frame.
    float g_bbW = 0.0f;
    float g_bbH = 0.0f;

    // Square corners, dark parchment-on-charcoal, brass accents — closer
    // to Skyrim's UI language than ImGui's default debug grey.
    void ApplyMenuStyle() {
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.WindowPadding = ImVec2(18.0f, 14.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.FramePadding = ImVec2(10.0f, 6.0f);
        auto* c = style.Colors;
        c[ImGuiCol_WindowBg]         = ImVec4(0.04f, 0.04f, 0.06f, 0.94f);
        c[ImGuiCol_ChildBg]          = ImVec4(0.06f, 0.06f, 0.08f, 0.55f);
        c[ImGuiCol_Border]           = ImVec4(0.55f, 0.50f, 0.35f, 0.60f);
        c[ImGuiCol_Text]             = ImVec4(0.91f, 0.89f, 0.85f, 1.00f);
        c[ImGuiCol_TextDisabled]     = ImVec4(0.58f, 0.55f, 0.47f, 1.00f);
        c[ImGuiCol_Header]           = ImVec4(0.34f, 0.29f, 0.16f, 0.85f);
        c[ImGuiCol_HeaderHovered]    = ImVec4(0.44f, 0.38f, 0.20f, 0.75f);
        c[ImGuiCol_HeaderActive]     = ImVec4(0.52f, 0.45f, 0.24f, 0.90f);
        c[ImGuiCol_Button]           = ImVec4(0.20f, 0.18f, 0.12f, 0.85f);
        c[ImGuiCol_ButtonHovered]    = ImVec4(0.40f, 0.35f, 0.19f, 0.90f);
        c[ImGuiCol_ButtonActive]     = ImVec4(0.52f, 0.45f, 0.24f, 1.00f);
        c[ImGuiCol_Separator]        = ImVec4(0.55f, 0.50f, 0.35f, 0.50f);
        c[ImGuiCol_ScrollbarBg]      = ImVec4(0.04f, 0.04f, 0.06f, 0.60f);
        c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.38f, 0.34f, 0.24f, 0.80f);
        c[ImGuiCol_TitleBg]          = ImVec4(0.04f, 0.04f, 0.06f, 1.00f);
        c[ImGuiCol_TitleBgActive]    = ImVec4(0.04f, 0.04f, 0.06f, 1.00f);
    }

    void DrawGemMenu() {
        auto& io = ImGui::GetIO();
        if (g_menu.wantClose.exchange(false)) {
            CloseGemMenu();
            return;
        }
        io.MouseDrawCursor = true;
        io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
        // Centered on each open (Appearing, not Always) so it can be
        // dragged afterwards; DisplaySize is backbuffer-true by now.
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.68f),
                                 ImGuiCond_Appearing);
        if (!ImGui::Begin("Gem Socketing", nullptr,
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            return;
        }
        {
            const char* title = "G E M   S O C K E T I N G";
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.70f, 0.45f, 1.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize(title).x) * 0.5f);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();
        }
        std::scoped_lock lk(g_menu.lock);
        const bool busy = g_menu.busy.load();
        if (busy) {
            ImGui::BeginDisabled();
        }
        const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
        const float half = ImGui::GetContentRegionAvail().x * 0.5f;
        ImGui::BeginChild("items", ImVec2(half - 6.0f, -footer), true);
        ImGui::TextDisabled("SOCKETABLE ITEMS");
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(g_menu.items.size()); ++i) {
            const auto& row = g_menu.items[i];
            std::string label = row.label;
            const int filled = (!row.slotGem[0].empty()) + (!row.slotGem[1].empty());
            if (filled > 0) {
                label += (filled == row.capacity) ? "  **" : "  *";  // * = partly, ** = full
            }
            label += std::format("##item{}", i);
            if (ImGui::Selectable(label.c_str(), g_menu.selItem == i)) {
                g_menu.selItem = i;
            }
        }
        if (g_menu.items.empty()) {
            ImGui::TextDisabled("No socketable items.");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("gems", ImVec2(0, -footer), true);
        if (g_menu.selItem >= 0 && g_menu.selItem < static_cast<int>(g_menu.items.size())) {
            const auto sel = g_menu.items[g_menu.selItem];  // copy: queue may rebuild
            ImGui::TextDisabled("%s%s", sel.label.c_str(),
                                sel.capacity > 1 ? "  [2 linked sockets]" : "");
            ImGui::Separator();
            // One line per socket slot. Filled = select to remove (+ station
            // feed/destroy per slot); empty = a free slot for the gem list below.
            int freeSlots = 0;
            for (int s = 0; s < sel.capacity && s < 2; ++s) {
                if (sel.slotGem[s].empty()) {
                    ++freeSlots;
                    ImGui::TextDisabled("Socket %d: empty", s + 1);
                    continue;
                }
                ImGui::PushID(s);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
                const std::string lbl =
                    std::format("Socket {}: {}  — select to remove", s + 1, sel.slotGem[s]);
                if (ImGui::Selectable(lbl.c_str())) {
                    const std::uint8_t slot = static_cast<std::uint8_t>(s);
                    QueueMenuTask([sel, slot]() { MenuUnsocket(sel.base, sel.uid, slot); });
                }
                ImGui::PopStyleColor();
                if (g_menu.station.load()) {
                    if (ImGui::SmallButton("Feed Soul")) {
                        const std::uint8_t slot = static_cast<std::uint8_t>(s);
                        QueueMenuTask([sel, slot]() { FeedSoulToGem(sel.base, sel.uid, slot); });
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Destroy")) {
                        const std::uint8_t slot = static_cast<std::uint8_t>(s);
                        QueueMenuTask([sel, slot]() { DestroyGem(sel.base, sel.uid, slot); });
                    }
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (freeSlots <= 0) {
                ImGui::TextDisabled(sel.capacity > 1 ? "Both sockets filled — remove one to swap."
                                                     : "Socket filled — select it above to remove.");
            } else {
                int shown = 0;
                for (int i = 0; i < static_cast<int>(g_menu.gems.size()); ++i) {
                    const auto gem = g_menu.gems[i];  // copy for the closure
                    if (gem.isArmor != sel.isArmor) {
                        continue;  // weapon gems only fit weapons; armor only armor
                    }
                    ++shown;
                    const std::string lbl = gem.label + std::format("##gem{}", i);
                    if (ImGui::Selectable(lbl.c_str())) {
                        QueueMenuTask([sel, gem]() {
                            MenuSocket(sel.base, sel.uid, gem.base, gem.uid);
                        });
                    }
                }
                if (shown == 0) {
                    ImGui::TextDisabled(sel.isArmor ? "No loose armor gems." : "No loose weapon gems.");
                }
            }
        } else {
            ImGui::TextDisabled("Select an item.");
        }
        ImGui::EndChild();
        if (busy) {
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Select an item, then a gem to socket it. Esc / B closes.");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::Button("Close")) {
            CloseGemMenu();
        }
        ImGui::End();
    }

    struct WndProcHook {
        static LRESULT thunk(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l) {
            if (a_msg == WM_KILLFOCUS && g_d3dReady.load()) {
                auto& io = ImGui::GetIO();
                io.ClearInputKeys();
            }
            return func(a_hwnd, a_msg, a_w, a_l);
        }
        static inline WNDPROC func = nullptr;
    };

    struct D3DInitHook {
        static void thunk() {
            func();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) {
                spdlog::error("[menu] renderer singleton missing — menu disabled");
                return;
            }
            auto* swapChain = renderer->data.renderWindows[0].swapChain;
            if (!swapChain) {
                spdlog::error("[menu] swapchain missing — menu disabled");
                return;
            }
            DXGI_SWAP_CHAIN_DESC sd{};
            if (swapChain->GetDesc(&sd) < 0) {
                spdlog::error("[menu] IDXGISwapChain::GetDesc failed — menu disabled");
                return;
            }
            g_device = renderer->data.forwarder;
            g_context = renderer->data.context;
            ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;  // never write imgui.ini into the game dir
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            if (!ImGui_ImplWin32_Init(sd.OutputWindow) || !ImGui_ImplDX11_Init(g_device, g_context)) {
                spdlog::error("[menu] ImGui backend init failed — menu disabled");
                return;
            }
            WndProcHook::func = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                sd.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
            ApplyMenuStyle();
            g_bbW = static_cast<float>(sd.BufferDesc.Width);
            g_bbH = static_cast<float>(sd.BufferDesc.Height);
            g_d3dReady.store(true);
            spdlog::info("[menu] ImGui initialized ({}x{})", sd.BufferDesc.Width, sd.BufferDesc.Height);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id = REL::RelocationID(75595, 77226);
        static constexpr auto                          offset = REL::VariantOffset(0x9, 0x275, 0x0);
    };

    struct DXGIPresentHook {
        static void thunk(std::uint32_t a_p1) {
            func(a_p1);
            if (!g_d3dReady.load() || !g_menu.open.load()) {
                return;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            if (g_bbW > 0.0f) {
                ImGui::GetIO().DisplaySize = ImVec2(g_bbW, g_bbH);
            }
            ImGui::NewFrame();
            DrawGemMenu();
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id = REL::RelocationID(75461, 77246);
        static constexpr auto                          offset = REL::Offset(0x9);
    };

    ImGuiKey DIKToImGuiKey(std::uint32_t a_dik) {
        switch (a_dik) {
        case 0xC8: return ImGuiKey_UpArrow;
        case 0xD0: return ImGuiKey_DownArrow;
        case 0xCB: return ImGuiKey_LeftArrow;
        case 0xCD: return ImGuiKey_RightArrow;
        case 0x1C: return ImGuiKey_Enter;
        case 0x12: return ImGuiKey_Enter;  // E = Skyrim activate
        default:   return ImGuiKey_None;
        }
    }

    ImGuiKey GamepadToImGuiKey(std::uint32_t a_key) {
        using K = RE::BSWin32GamepadDevice::Key;
        switch (static_cast<K>(a_key)) {
        case K::kUp:    return ImGuiKey_GamepadDpadUp;
        case K::kDown:  return ImGuiKey_GamepadDpadDown;
        case K::kLeft:  return ImGuiKey_GamepadDpadLeft;
        case K::kRight: return ImGuiKey_GamepadDpadRight;
        case K::kA:     return ImGuiKey_GamepadFaceDown;
        case K::kX:     return ImGuiKey_GamepadFaceLeft;
        case K::kY:     return ImGuiKey_GamepadFaceUp;
        default:        return ImGuiKey_None;
        }
    }

    // Input dispatch thunk: while the menu is open, feed ImGui and swallow
    // everything so the game world stays deaf to it.
    struct InputDispatchHook {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events) {
            if (!a_events || !g_menu.open.load() || !g_d3dReady.load()) {
                func(a_source, a_events);
                return;
            }
            auto&        io = ImGui::GetIO();
            static float cursorX = -1.0f;
            static float cursorY = -1.0f;
            if (cursorX < 0.0f) {
                cursorX = io.DisplaySize.x * 0.5f;
                cursorY = io.DisplaySize.y * 0.5f;
            }
            for (auto* e = *a_events; e; e = e->next) {
                if (e->eventType == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    auto* m = static_cast<RE::MouseMoveEvent*>(e);
                    cursorX = std::clamp(cursorX + static_cast<float>(m->mouseInputX), 0.0f,
                                         io.DisplaySize.x);
                    cursorY = std::clamp(cursorY + static_cast<float>(m->mouseInputY), 0.0f,
                                         io.DisplaySize.y);
                    io.AddMousePosEvent(cursorX, cursorY);
                } else if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                    auto* b = static_cast<RE::ButtonEvent*>(e);
                    if (!b->IsDown() && !b->IsUp()) {
                        continue;
                    }
                    const bool          down = b->IsDown();
                    const std::uint32_t code = b->GetIDCode();
                    switch (b->device.get()) {
                    case RE::INPUT_DEVICE::kMouse:
                        if (code <= 4) {
                            io.AddMouseButtonEvent(static_cast<int>(code), down);
                        } else if (code == 8 && down) {  // wheel up
                            io.AddMouseWheelEvent(0.0f, 1.0f);
                        } else if (code == 9 && down) {  // wheel down
                            io.AddMouseWheelEvent(0.0f, -1.0f);
                        }
                        break;
                    case RE::INPUT_DEVICE::kKeyboard:
                        if ((code == 0x01 || code == 0x0F) && down) {  // Esc / Tab
                            g_menu.wantClose = true;
                        } else if (auto key = DIKToImGuiKey(code); key != ImGuiKey_None) {
                            io.AddKeyEvent(key, down);
                        }
                        break;
                    case RE::INPUT_DEVICE::kGamepad:
                        if (static_cast<RE::BSWin32GamepadDevice::Key>(code) ==
                                RE::BSWin32GamepadDevice::Key::kB &&
                            down) {
                            g_menu.wantClose = true;
                        } else if (auto key = GamepadToImGuiKey(code); key != ImGuiKey_None) {
                            io.AddKeyEvent(key, down);
                        }
                        break;
                    default:
                        break;
                    }
                }
                // thumbstick / char events: swallowed silently for now
            }
            *a_events = nullptr;  // the game sees no input while the menu is open
            func(a_source, a_events);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id = REL::RelocationID(67315, 68617);
        static constexpr auto                          offset = REL::Offset(0x7B);
    };

    template <class T>
    void write_thunk_call() {
        auto&                                 trampoline = SKSE::GetTrampoline();
        const REL::Relocation<std::uintptr_t> hook{ T::id, T::offset };
        T::func = trampoline.write_call<5>(hook.address(), T::thunk);
    }

    void Install() {
        SKSE::AllocTrampoline(64);
        write_thunk_call<D3DInitHook>();
        write_thunk_call<DXGIPresentHook>();
        write_thunk_call<InputDispatchHook>();
        spdlog::info("[menu] render + input hooks installed");
    }

}  // namespace menuhook

void OpenGemMenu(bool a_station) {
    if (!menuhook::g_d3dReady.load()) {
        spdlog::error("[menu] open requested but ImGui never initialized");
        Notify("The Gem Pouch menu is unavailable — see MEO.log.");
        return;
    }
    if (g_menu.open.load()) {
        return;
    }
    BuildMenuSnapshot();
    g_menu.selItem = 0;
    g_menu.wantClose = false;
    g_menu.station = a_station;
    g_menu.open = true;
    spdlog::info("[menu] opened ({}): {} item(s), {} gem(s)", a_station ? "station" : "pouch",
                 g_menu.items.size(), g_menu.gems.size());
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
    if (g_corpseGems.empty() || g_gemDropChance <= 0.0f) {
        return;
    }
    if (std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) >= g_gemDropChance) {
        return;
    }
    const auto& rg = g_gems[g_corpseGems[
        std::uniform_int_distribution<std::size_t>(0, g_corpseGems.size() - 1)(g_rng)]];
    const int lvl = (std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) < g_gemLevel2Chance)
                        ? 2 : 1;
    if (auto* item = rg.items[lvl - 1]) {
        a_victim->AddObjectToContainer(item, nullptr, 1, nullptr);
        spdlog::info("[loot] corpse gem '{}' {} on {:08X}", rg.def->gid, meo::kRoman[lvl - 1],
                     a_victim->GetFormID());
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
    if (!IsSocketableWeaponBase(base->As<RE::TESObjectWEAP>())) {
        return;  // base-enchanted, non-playable, unarmed, bound, or artifact
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
    // Deterministic level roll: fGemLevel2Chance of these are born level II.
    const std::uint32_t l2cut = static_cast<std::uint32_t>(g_gemLevel2Chance * 10000.0f);
    const int level = (HashU32(h ^ 0x5A5A5A5Au) % 10000) < l2cut ? 2 : 1;
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
            SKSE::GetTaskInterface()->AddTask([]() { OpenGemMenu(); });
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
    // Armor starter set (v6 flag — grants once, even on saves that already
    // burned the weapon-starter flag). Enough for dual-glove testing + variety.
    if (!g_armorStarterGranted) {
        int given = 0;
        for (const char* gid : { "fortifyhealth", "fortifymagicka", "fortifystamina",
                                 "resistfire", "fortifydestruction" }) {
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
        if (given > 0) {
            g_armorStarterGranted = true;
            spdlog::info("armor starter kit: {} level-I gems granted", given);
            Notify("Armor gems added to your inventory.");
        }
    }
}

// On load, worn socketed gems' abilities are runtime state that doesn't persist
// — re-stamp + re-activate every worn socketed item so effects are live without
// a manual re-equip. (The g_sockets records + item ExtraEnchantment do persist;
// the magic caster does not.)
//
// REGRESSION (m15): the m12 never-drain mechanism (costOverride=0 + kCostOverride
// on the created enchant) is a runtime property that does NOT survive save/load;
// on load the socket reverts to auto per-hit cost and, at our scaled-up magnitude,
// charge-starves exactly like the pre-m12 firing bug — so the enchant shows but
// never fires. Pre-m11 builds never hit this (low magnitude, charge 500 sufficed,
// nothing to lose on load). Re-stamping (a_rebuild) re-mints the enchant with the
// override restored + re-derives the worn ability.
//
// TIMING: kPostLoadGame fires while the engine is still finalizing the loaded
// actor's equipped-weapon process, so a re-stamp applied then is silently
// discarded (the player's manual re-socket, minutes later, is what fixed it).
// So we re-stamp on a few delays until the actor is live. Rebuild every attempt
// (AddWeaponEnchantment dedupes identical created enchants — no churn) so the
// final state is correct regardless of the engine's load-finalization order.
// Each call re-scans inventory so captured xList pointers can't go stale.
void ReapplyWornSockets(bool a_rebuild, bool a_diag = false) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }
    int n = 0;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists ||
            !(entry->object->Is(RE::FormType::Weapon) || entry->object->Is(RE::FormType::Armor))) {
            continue;
        }
        for (auto* xl : *entry->extraLists) {
            if (!xl || !IsWornXList(xl)) {
                continue;
            }
            auto* xid = xl->GetByType<RE::ExtraUniqueID>();
            if (!xid) {
                continue;
            }
            bool ours = false;
            for (int s = 0; s < kMaxSockets; ++s) {
                if (g_sockets.contains(MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                               static_cast<std::uint8_t>(s)))) {
                    ours = true;
                    break;
                }
            }
            if (!ours) {
                continue;
            }
            // m15 diagnostic: on the immediate post-load pass, read the enchant
            // that SURVIVED the save BEFORE we overwrite it — proves whether the
            // never-drain costOverride/kCostOverride round-trips (hypothesis) or
            // it's purely an ability-derivation timing issue.
            if (a_diag) {
                auto* xEnch = xl->GetByType<RE::ExtraEnchantment>();
                auto* en = xEnch ? xEnch->enchantment : nullptr;
                spdlog::info("[load-diag] {:08X}/{} as-loaded: ench={:08X} kCostOverride={} "
                             "costOverride={} charge={}",
                             entry->object->GetFormID(), xid->uniqueID,
                             en ? en->GetFormID() : 0u,
                             en && en->data.flags.any(
                                       RE::EnchantmentItem::EnchantmentFlag::kCostOverride),
                             en ? en->data.costOverride : -1,
                             xEnch ? xEnch->charge : 0);
            }
            if (a_rebuild) {
                RebuildInstanceEnchant(entry->object, xl);
            }
            ApplyWornAbility(player, entry->object, xl, xl->HasType(RE::ExtraDataType::kWornLeft));
            ++n;
        }
    }
    spdlog::info("[load] {} worn socketed item(s) (rebuild={})", n, a_rebuild);
}

// Fire the reapply once immediately (rebuild) then again after the loaded actor
// settles — the immediate pass is usually discarded (see ReapplyWornSockets).
// A detached timer thread hands each retry back to the main thread via a task.
void ScheduleReapplyWornSockets() {
    SKSE::GetTaskInterface()->AddTask([]() { ReapplyWornSockets(true, /*diag=*/true); });
    std::thread([]() {
        for (int ms : { 1500, 2500, 4000 }) {  // cumulative ~1.5s, 4s, 8s post-load
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            SKSE::GetTaskInterface()->AddTask([]() { ReapplyWornSockets(true); });
        }
    }).detach();
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
    const std::uint8_t armorStarter = g_armorStarterGranted ? 1 : 0;  // v6 field
    a_intfc->WriteRecordData(armorStarter);
    const std::uint32_t count = static_cast<std::uint32_t>(g_sockets.size());
    a_intfc->WriteRecordData(count);
    for (const auto& [key, rec] : g_sockets) {
        const std::uint32_t baseID = static_cast<std::uint32_t>(key >> 24);
        const std::uint16_t uid = static_cast<std::uint16_t>((key >> 8) & 0xFFFF);
        const std::uint8_t  slot = static_cast<std::uint8_t>(key & 0xFF);
        a_intfc->WriteRecordData(baseID);
        a_intfc->WriteRecordData(uid);
        a_intfc->WriteRecordData(slot);   // v5
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
    g_armorStarterGranted = false;
    CloseGemMenu();
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
        if (version >= 6) {  // v6: armorStarterGranted (older saves default false)
            std::uint8_t armorStarter = 0;
            a_intfc->ReadRecordData(armorStarter);
            g_armorStarterGranted = armorStarter != 0;
        }
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t baseID = 0;
            std::uint16_t uid = 0;
            std::uint8_t  slot = 0;
            SocketRecord  rec{};
            std::uint16_t len = 0;
            a_intfc->ReadRecordData(baseID);
            a_intfc->ReadRecordData(uid);
            if (version >= 5) {  // v5: per-socket slot; v3/v4 records are all slot 0
                a_intfc->ReadRecordData(slot);
            }
            a_intfc->ReadRecordData(rec.level);
            a_intfc->ReadRecordData(rec.xp);
            a_intfc->ReadRecordData(len);
            rec.gid.resize(len);
            a_intfc->ReadRecordData(rec.gid.data(), len);
            g_sockets[MakeKey(baseID, uid, slot)] = std::move(rec);
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
    g_armorStarterGranted = false;
    CloseGemMenu();
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
            console->Print("MEO native v0.23.0 (M15 load-reapply timing/costOverride fix) loaded");
        }
        spdlog::info("kDataLoaded: MEO M6 live; SpellCast + Death + CellAttach + CrosshairRef sinks + render/input hooks");
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        // After LoadCallback/Revert — co-save flags are current here.
        SKSE::GetTaskInterface()->AddTask([]() {
            EnsurePlayerSetup();
            RefreshPerks();
        });
        ScheduleReapplyWornSockets();  // re-activate worn gem effects (deferred + retried)
        break;
    default:
        break;
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    menuhook::Install();  // must be written before the renderer initializes

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.23.0 (M15: on-load reapply timing + costOverride-loss fix) loading; runtime {}", gameVersion.string());
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
