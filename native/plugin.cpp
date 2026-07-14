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
// wingdi.h #defines GetObject -> GetObjectW, which hijacks
// RE::BGSDefaultObjectManager::GetObject<T>(). Drop the macro.
#ifdef GetObject
#    undef GetObject
#endif

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
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
constexpr std::uint32_t kSerVersion = 11;  // v11: + discoveredGems. v10: handPlacedMask. v9: supportScaffold.

// ── Catalog resolved against the live load order (kDataLoaded) ───────
constexpr const char* kPluginName = "MEO.esp";
constexpr RE::FormID  kPouchSpellID = 0x803;  // MEO.esp-local
constexpr RE::FormID  kEchoShareSpellID = 0x809;  // m36: Echo armor follower-share (effect swapped at runtime)

// m21 recipe riders; m22: rider set comes from the per-list calibration file
// when it names the family (installer derives it from the list's own generic
// recipe lines), else from the compiled catalog defaults.
struct RtRider {
    RE::EffectSetting* mgef = nullptr;
    float              ratio = 0.0f;
    float              absMag = 0.0f;  // m32: flat-magnitude rider
    float              dur = 0.0f;
};

struct ResolvedGem {
    const meo::GemDef*                  def = nullptr;
    RE::EffectSetting*                  mgef = nullptr;   // null = disabled (missing master)
    std::array<RE::EffectSetting*, 5>   mgefLv{};  // m28: rank ladder (defaults to mgef)
    std::vector<std::pair<int, std::string>> lvNotes;  // m29: "level N unlocks ..." lines
    std::string                         liveName;  // m27: winning MGEF renamed the effect
    std::array<RtRider, 4>              riders{};  // m25: Squire-style recipes carry 3
    int                                 nRiders = 0;
    std::array<RE::TESObjectMISC*, 5>   items{};
};
std::vector<ResolvedGem>                          g_gems;
// m27 (marth confirmed the 'pen' in game): Requiem-style lists REPURPOSE
// vanilla MGEFs at their original FormKeys (0x07A0FB 'Fortify Light Armor'
// is now 'Fortify Armor Penetration'), so catalog labels can lie. The gem's
// spoken name follows the WINNING record: relabel when neither name
// contains the other (formatting differences like 'Fire' vs 'Fire Damage'
// stay; semantic swaps don't).
const char* GemName(const ResolvedGem& a_rg) {
    return a_rg.liveName.empty() ? a_rg.def->name : a_rg.liveName.c_str();
}
std::unordered_map<RE::FormID, std::pair<int, int>> g_gemByItem;  // item -> {gemIdx, level}
std::unordered_map<std::string_view, int>         g_gemByGid;
std::vector<int>                                  g_lootableGems;  // weapon-domain, world-weapon stamps
std::vector<int>                                  g_corpseGems;    // weapon+armor, corpse drops
std::vector<int>                                  g_supportGems;   // m36h: support gems (boss-loot pool)
RE::SpellItem*                                    g_pouchSpell = nullptr;
RE::SpellItem*                                    g_echoShareSpell = nullptr;  // m36: Echo armor follower-share

// ── m19: themed NPC spawn pools (DESIGN §3 "Post-strip gem economy") ──
// Enemy archetype × gem theme weights build per-archetype weighted pools
// (index 0 = weapon-domain gems, 1 = armor-domain). S-tier (spawnWeight 1)
// takes an extra ×0.5 on enemies — rarer than world drops, per design.
enum class Arch : int { kWarrior = 0, kMage, kRogue, kUndead, kCount };
// [arch][theme]; theme order = meo::Theme (Fire Frost Shock Arcane Drain
// Martial Roguish Holy Utility).
constexpr float kArchThemeW[4][9] = {
    /*Warrior*/ { 0.30f, 0.30f, 0.30f, 0.05f, 0.20f, 1.00f, 0.20f, 0.10f, 0.0f },
    /*Mage*/    { 0.70f, 0.70f, 0.70f, 1.00f, 0.50f, 0.10f, 0.10f, 0.20f, 0.0f },
    /*Rogue*/   { 0.20f, 0.20f, 0.20f, 0.10f, 0.50f, 0.40f, 1.00f, 0.00f, 0.0f },
    /*Undead*/  { 0.15f, 1.00f, 0.15f, 0.40f, 0.80f, 0.50f, 0.10f, 0.00f, 0.0f },
};
std::vector<int> g_npcPool[4][2];       // [arch][isArmor] -> weighted gem indices
RE::BGSKeyword*  g_kwNPC = nullptr;     // ActorTypeNPC (humanoids only)
RE::BGSKeyword*  g_kwUndead = nullptr;  // ActorTypeUndead
float g_magnitudeMult = 1.0f;  // [XP] fMagnitudeMult master power scale; used by StampInstance below, set by ReadConfig (MCM)

// ── M3d forms (all IDs extracted from the real Lorerim masters) ───────
constexpr RE::FormID kPouchContID = 0x8FE;        // MEO.esp CONT (frozen) — M5 Gem Pouch menu
constexpr RE::FormID kMentorGemID = 0x8FF;        // MEO.esp MISC (frozen, outside gem range)
constexpr RE::FormID kSoulCairnWorldID = 0x001408;// Dawnguard.esm WRLD DLC01SoulCairn
constexpr RE::FormID kBossLocRefTypeID = 0x0130F7;// Skyrim.esm LCRT "Boss"
constexpr RE::FormID kDragonKeywordID = 0x035D59; // Skyrim.esm KYWD ActorTypeDragon
constexpr RE::FormID kReusableSoulGemKW = 0x0ED2F1;// Skyrim.esm KYWD ReusableSoulGem
RE::TESObjectCONT*      g_pouchCont = nullptr;
// m27 (marth: gems must not clutter the player's inventory): every gem the
// player owns lives in a hidden persistent container ref — the Gem Pouch
// made literal. Created once per save (co-save v7 keeps the ref id), gems
// route here on arrival; the menu reads it; socketing consumes from it.
RE::FormID g_pouchRefID = 0;
bool       g_pouchCreatedThisLoad = false;  // gates the stranded-gem recovery
RE::TESObjectREFR* PouchRef() {
    if (!g_pouchRefID) {
        return nullptr;
    }
    auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_pouchRefID);
    return (ref && ref->GetBaseObject() == g_pouchCont && !ref->IsDeleted()) ? ref : nullptr;
}
void EnsurePouchRef() {
    if (PouchRef() || !g_pouchCont) {
        return;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    // forcePersist = TRUE (2026-07-10 field loss: a temp+disabled ref was
    // engine-purged mid-play FOUR times in one session, annihilating its
    // contents — 17 gems. Only a genuinely persistent ref survives.)
    auto ref = player->PlaceObjectAtMe(g_pouchCont, true);
    if (!ref) {
        spdlog::error("[pouch] container ref creation failed - gems stay in inventory");
        return;
    }
    ref->formFlags |= RE::TESObjectREFR::RecordFlags::kPersistent;
    ref->Disable();  // never visible
    const bool recreated = g_pouchRefID != 0;
    g_pouchRefID = ref->GetFormID();
    g_pouchCreatedThisLoad = true;
    spdlog::info("[pouch] hidden gem container created {:08X}{}", g_pouchRefID,
                 recreated ? " (previous ref DEAD — recovery pass will run)" : "");
}
// m32c: when the pouch had to be recreated, its contents died with the old
// ref — but the RECORDS live in the co-save. Any loose-gem record whose uid
// exists in neither the player nor the pouch is re-minted with its banked
// level and XP. Runs only on pouch creation; healthy loads never trigger it.
void GiveGemInstance(int a_gemIdx, int a_level, float a_xp);
void Notify(const std::string& a_msg);
void RecoverStrandedGems() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* pouch = PouchRef();
    if (!player || !pouch) {
        return;
    }
    std::unordered_set<InstKey> live;
    auto collect = [&](RE::TESObjectREFR* a_h) {
        for (const auto& [obj, data] : a_h->GetInventory(
                 [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
            if (data.first <= 0 || !g_gemByItem.contains(obj->GetFormID()) ||
                !data.second || !data.second->extraLists) {
                continue;
            }
            for (auto* xl : *data.second->extraLists) {
                if (auto* xid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr) {
                    live.insert(MakeKey(obj->GetFormID(), xid->uniqueID));
                }
            }
        }
    };
    collect(player);
    collect(pouch);
    struct Rz { int idx; int level; float xp; InstKey key; };
    std::vector<Rz> res;
    for (const auto& [key, rec] : g_sockets) {
        if ((key & 0xFF) != 0) {
            continue;  // gem instances bank in slot 0
        }
        const auto base = static_cast<RE::FormID>(key >> 24);
        if (!g_gemByItem.contains(base) || live.contains(key)) {
            continue;  // not a loose-gem record, or its gem is accounted for
        }
        auto gemIt = g_gemByGid.find(rec.gid);
        if (gemIt == g_gemByGid.end()) {
            continue;
        }
        res.push_back({ gemIt->second, rec.level, rec.xp, key });
    }
    for (const auto& r : res) {
        spdlog::info("[pouch] RECOVERING stranded gem '{}' L{} xp={:.0f}",
                     g_gems[r.idx].def->gid, r.level, r.xp);
        g_sockets.erase(r.key);
        GiveGemInstance(r.idx, r.level, r.xp);
    }
    if (!res.empty()) {
        Notify(std::format("MEO: recovered {} lost gem(s) into your pouch, XP intact.",
                           res.size()));
        spdlog::info("[pouch] {} stranded gem(s) recovered", res.size());
    }
}

RE::ExtraDataList* FindInstanceXList(RE::TESObjectREFR* a_owner, RE::TESBoundObject* a_form,
                                    std::uint16_t a_uid);  // defined below

// m36l (marth cleanup): one-shot removal of ALL loose support gems from the
// player + the pouch, for wiping test-scaffold gems off a save. Dev-only,
// gated on bPurgeSupportGems, run once per load. Erases their loose (slot-0)
// co-save records first so RecoverStrandedGems can't re-add them. Socketed
// support gems are left alone (unsocket them first if you want those gone too).
void PurgeLooseSupportGems() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    // Drop loose support-gem records (slot 0) so recovery won't resurrect them.
    std::erase_if(g_sockets, [](const auto& kv) {
        if ((kv.first & 0xFF) != 0) {
            return false;  // socketed slot — keep
        }
        auto gi = g_gemByGid.find(kv.second.gid);
        return gi != g_gemByGid.end() && g_gems[gi->second].def->isSupport;
    });
    int removed = 0;
    auto purge = [&](RE::TESObjectREFR* h) {
        if (!h) {
            return;
        }
        std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> hits;
        for (const auto& [obj, data] : h->GetInventory(
                 [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
            auto it = g_gemByItem.find(obj->GetFormID());
            if (data.first > 0 && it != g_gemByItem.end() &&
                g_gems[it->second.first].def->isSupport) {
                hits.emplace_back(obj, data.first);
            }
        }
        for (auto& [obj, n] : hits) {
            h->RemoveItem(obj, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            removed += n;
        }
    };
    purge(player);
    purge(PouchRef());
    if (removed > 0) {
        spdlog::info("[purge] removed {} loose support gem(s) from inventory + pouch", removed);
        Notify(std::format("MEO: removed {} support gem(s).", removed));
    } else {
        spdlog::info("[purge] no loose support gems to remove");
    }
}

void RouteGemsToPouch() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* pouch = PouchRef();
    if (!player || !pouch) {
        return;
    }
    // Snapshot first (moving invalidates the inventory iterator). XP instances
    // (uid + slot-0 record) move ONE AT A TIME so each hop's uid rewrite is
    // unambiguous; plain gems (no record) batch freely.
    struct Inst { RE::TESBoundObject* obj; std::uint16_t uid; SocketRecord rec; };
    std::vector<Inst>                                     instances;
    std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> plains;
    for (const auto& [obj, data] : player->GetInventory(
             [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
        if (data.first <= 0 || !g_gemByItem.contains(obj->GetFormID())) {
            continue;
        }
        std::int32_t plain = data.first;
        if (data.second && data.second->extraLists) {
            for (auto* xl : *data.second->extraLists) {
                auto* xid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr;
                if (!xid) {
                    continue;
                }
                auto recIt = g_sockets.find(MakeKey(obj->GetFormID(), xid->uniqueID));
                if (recIt != g_sockets.end()) {
                    instances.push_back({ obj, xid->uniqueID, recIt->second });
                    plain -= std::max(xl->GetCount(), 1);
                }
            }
        }
        if (plain > 0) {
            plains.emplace_back(obj, plain);
        }
    }
    int moved = 0;
    for (auto& [obj, n] : plains) {
        player->RemoveItem(obj, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, pouch);
        moved += n;
    }
    // The engine rewrites a moved instance's uid (m19), and MISC gems aren't
    // enchanted so the event-driven rekey (enchant-only) never followed them:
    // the record stranded and the gem read as PLAIN, merging with the no-XP
    // stack (marth 2026-07-12). Diffing the pouch's uid-set across a single
    // move pins the arriving uid deterministically — rewrite or not — so the
    // record follows by hand, here, for MISC gems.
    auto pouchUids = [&](RE::FormID a_base, std::vector<std::uint16_t>& out) {
        auto* ch = pouch->GetInventoryChanges();
        if (!ch || !ch->entryList) {
            return;
        }
        for (auto* e : *ch->entryList) {
            if (!e || !e->object || e->object->GetFormID() != a_base || !e->extraLists) {
                continue;
            }
            for (auto* xl : *e->extraLists) {
                if (auto* xid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr) {
                    out.push_back(xid->uniqueID);
                }
            }
        }
    };
    for (auto& in : instances) {
        const RE::FormID base = in.obj->GetFormID();
        auto* xl = FindInstanceXList(player, in.obj, in.uid);
        if (!xl) {
            continue;  // vanished mid-pass; RecoverStrandedGems reclaims the record
        }
        std::vector<std::uint16_t> before;
        pouchUids(base, before);
        g_sockets.erase(MakeKey(base, in.uid));
        player->RemoveItem(in.obj, 1, RE::ITEM_REMOVE_REASON::kRemove, xl, pouch);
        std::vector<std::uint16_t> after;
        pouchUids(base, after);
        std::uint16_t newUid = in.uid;
        for (auto u : after) {
            if (std::find(before.begin(), before.end(), u) == before.end()) {
                newUid = u;
                break;
            }
        }
        g_sockets[MakeKey(base, newUid)] = in.rec;
        ++moved;
        if (newUid != in.uid) {
            spdlog::info("[pouch] gem {:08X} record followed uid {} -> {} (L{} xp {:.0f})",
                         base, in.uid, newUid, in.rec.level, in.rec.xp);
        }
    }
    if (moved > 0) {
        spdlog::info("[pouch] {} gem(s) tucked away", moved);
    }
}
RE::TESObjectMISC*      g_mentorGem = nullptr;
RE::TESWorldSpace*      g_soulCairn = nullptr;    // null = Dawnguard absent
RE::BGSLocationRefType* g_bossRefType = nullptr;
RE::BGSKeyword*         g_dragonKeyword = nullptr;
RE::BGSKeyword*         g_reusableSoulGemKW = nullptr;
bool                    g_mentorGranted = false;  // co-save v4
bool                    g_armorStarterGranted = false;  // co-save v6
// DESIGN §3 soul-feed Gem XP by soul size (petty..grand; black counts grand).
// m26b (marth 2026-07-10): soul feeding was power-leveling gems (a grand
// soul = half a level-I threshold) — cut ~80%. Socketed gem POWER is
// untouched ("skill gems are perfect"); this is only the soul->XP rate.
// Destroy-reclaim uses the same table, so the soul<->XP exchange stays
// symmetric. Enchanting SKILL xp per soul (kSoulSkillXP) unchanged.
constexpr float kSoulFeedXP[5] = { 1.0f, 2.5f, 5.0f, 12.0f, 40.0f };
// Filled vanilla soul gems (Skyrim.esm), petty..grand — gem destruction
// reclaims 1/10 of banked Gem XP into the largest of these it can afford.
constexpr RE::FormID kFilledSoulGemIDs[5] = { 0x02E4E3, 0x02E4E5, 0x02E4F3, 0x02E4FB, 0x02E4FF };
RE::TESSoulGem*      g_filledSoulGems[5] = {};

// MEO perks (DESIGN §6). MEO.esp-local FormIDs 0x810.. — see MEO_GenerateESP.
constexpr RE::FormID kPerkAttuneBase = 0x810;  // 0x810..0x814 = Attunement 1..5
constexpr RE::FormID kPerkGemCutter  = 0x815;
constexpr RE::FormID kPerkSoulFeeder = 0x816;
constexpr RE::FormID kPerkTwinned    = 0x817;  // Twinned Fitting: chest 2nd socket
constexpr RE::FormID kPerkJeweler    = 0x818;  // Master Jeweler: weapon 2nd socket
constexpr RE::FormID kPerkPyrestone  = 0x819;  // Pyrestone Affinity: Fire/Chaos +25% (m34)
constexpr RE::FormID kPerkFroststone = 0x81A;  // Froststone Affinity: Frost/Chaos +25%
constexpr RE::FormID kPerkStormstone = 0x81B;  // Stormstone Affinity: Shock/Chaos +25%
constexpr RE::FormID kPerkFacet      = 0x81C;  // Facet Insight: skill/attribute armor gems +25%
RE::BGSPerk* g_perkAttune[5] = {};
RE::BGSPerk* g_perkGemCutter = nullptr;
RE::BGSPerk* g_perkSoulFeeder = nullptr;
RE::BGSPerk* g_perkTwinned = nullptr;
RE::BGSPerk* g_perkJeweler = nullptr;
RE::BGSPerk* g_perkPyrestone = nullptr;   // m34 elemental affinities + Facet Insight
RE::BGSPerk* g_perkFroststone = nullptr;
RE::BGSPerk* g_perkStormstone = nullptr;
RE::BGSPerk* g_perkFacet = nullptr;
// m33: vanilla/Requiem "Arcane Blacksmith" (Skyrim.esm 0x05218E, overridden
// in place by Requiem). The engine hardcodes tempering of ENCHANTED items to
// this perk by FormID (it carries no entry points), so the only way to let
// socketed weapons be improved without it is to grant it. MEO converts all
// generic enchanted loot to sockets, so in practice this only frees socketed
// gear (+ artifacts) — marth's ruling 2026-07-12.
RE::BGSPerk* g_perkArcaneBlacksmith = nullptr;
bool         g_meoGrantedArcane = false;  // co-save v8: MEO added the perk (revocable)
bool         g_supportScaffoldGranted = false;  // co-save v9: test-scaffold support gems handed out
std::uint8_t g_handPlacedMask = 0;              // co-save v10: bitmask of hand-placed support gems dropped (m36i)
std::unordered_set<std::string> g_discoveredGems;  // co-save v11: gem gids the player has studied (m37 Enchanting XP)
bool g_needSeedDiscoveries = false;             // m37: pre-v11 save loaded — seed held gems silently, no XP burst
bool g_treeMode = false;  // MEO - Patch.esp installed: perks come from the tree, not auto-grant
// Cached from the player's perks (refreshed on load + menu close).
int  g_attuneRank = 0;      // 0..5 → +8% gem magnitude per rank
bool g_hasGemCutter = false;  // +50% Gem XP
bool g_hasSoulFeeder = false; // soul feeding is twice as potent
bool g_hasTwinned = false;    // chest armor holds 2 linked gems
bool g_hasJeweler = false;    // weapons hold 2 linked gems
bool g_hasPyrestone = false;  // Fire/Chaos +25% (m34)
bool g_hasFroststone = false; // Frost/Chaos +25%
bool g_hasStormstone = false; // Shock/Chaos +25%
bool g_hasFacet = false;      // skill/attribute armor gems +25%

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

// ── m22: per-list calibration (installer-derived rider recipes) ───────
// The installer reads the load order's own generic loot lines and writes
// SKSE/Plugins/MEO/meo_calibration.json. A family named there gets EXACTLY
// that rider set (an empty list clears the compiled default — the list's
// recipe is authoritative); families absent keep compiled defaults.
struct CalRider {
    std::string plugin;
    RE::FormID  fid = 0;
    float       ratio = 0.0f;   // ratio mode: magnitude = primary x ratio
    float       absMag = 0.0f;  // m32 absolute mode: flat magnitude (dur-anchored primaries)
    float       dur = 0.0f;
};
std::unordered_map<std::string, std::vector<CalRider>> g_calibration;
// m28: rank ladder — gem level -> the list's kin MGEF of that tier (higher
// ranks carry protection keywords the list's spells check on the target).
struct CalLevel { std::string plugin; RE::FormID fid = 0; };
std::unordered_map<std::string, std::array<CalLevel, 5>> g_calLevels;
// m35b: per-list magnitude curve derived by the installer from THIS load
// order's own enchant strengths (marth: derive, don't hardcode). Overrides
// the compiled catalog curve when present; families absent keep the default.
std::unordered_map<std::string, std::array<float, 5>> g_calCurves;

// m23 loot conversion (marth: covered enchanted generics CONVERT, never
// vanish): at spawn/acquire, "Iron Sword of Embers" becomes an Iron Sword
// with a Fire gem socketed and ACTIVE, level I/II rolled with the same
// sliders as loose drops. The installer derives this table per list.
struct CalConversion {
    std::string plugin;
    RE::FormID  fid = 0;
    std::string basePlugin;
    RE::FormID  baseFid = 0;
    std::string family;
};
std::vector<CalConversion> g_calConversions;

struct ConvTarget {
    RE::TESBoundObject* base = nullptr;
    int                 gemIdx = -1;
};
std::unordered_map<RE::FormID, ConvTarget> g_convert;  // runtime item id -> target

static void LoadCalibration() {
    constexpr const char* kPath = "Data/SKSE/Plugins/MEO/meo_calibration.json";
    std::ifstream f(kPath);
    if (!f) {
        spdlog::info("no calibration file ({}) — compiled rider defaults in force", kPath);
        return;
    }
    // m35: reset before load so a re-read never accumulates stale entries.
    g_calibration.clear();
    g_calLevels.clear();
    g_calCurves.clear();
    g_calConversions.clear();
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& ex) {
        spdlog::error("calibration JSON unreadable ({}): {} — compiled defaults in force",
                      kPath, ex.what());
        return;
    }
    // m35: families and conversions parse INDEPENDENTLY — a typo in one
    // family's rider must not take the whole 10k-row conversion table down
    // with it (that reverts loot conversion to nothing, silently).
    try {
        for (const auto& [fam, val] : j.at("families").items()) {
            std::vector<CalRider> rs;
            for (const auto& r : val.at("riders")) {
                CalRider cr;
                cr.plugin = r.at("plugin").get<std::string>();
                cr.fid = static_cast<RE::FormID>(
                    std::stoul(r.at("fid").get<std::string>(), nullptr, 16));
                cr.ratio = r.value("ratio", 0.0f);
                cr.absMag = r.value("mag", 0.0f);  // m32: absolute recipes
                cr.dur = r.value("dur", 0.0f);
                rs.push_back(std::move(cr));
            }
            g_calibration[fam] = std::move(rs);
            if (val.contains("levels")) {
                std::array<CalLevel, 5> lv{};
                int i = 0;
                for (const auto& l : val.at("levels")) {
                    if (i >= 5) {
                        break;
                    }
                    lv[i].plugin = l.at("plugin").get<std::string>();
                    lv[i].fid = static_cast<RE::FormID>(
                        std::stoul(l.at("fid").get<std::string>(), nullptr, 16));
                    ++i;
                }
                g_calLevels[fam] = lv;
            }
            if (val.contains("curve")) {  // m35b: per-list magnitude curve
                std::array<float, 5> cv{};
                int i = 0;
                for (const auto& m : val.at("curve")) {
                    if (i >= 5) {
                        break;
                    }
                    cv[i++] = m.get<float>();
                }
                if (i == 5) {
                    g_calCurves[fam] = cv;
                }
            }
        }
    } catch (const std::exception& ex) {
        spdlog::error("calibration families parse failed ({}): {} — riders/ladders default",
                      kPath, ex.what());
        g_calibration.clear();
        g_calLevels.clear();   // m35: keep ladders and riders consistent
        g_calCurves.clear();
    }
    try {
        if (j.contains("conversions")) {
            for (const auto& c : j.at("conversions")) {
                CalConversion cc;
                cc.plugin = c.at("plugin").get<std::string>();
                cc.fid = static_cast<RE::FormID>(
                    std::stoul(c.at("fid").get<std::string>(), nullptr, 16));
                cc.basePlugin = c.at("basePlugin").get<std::string>();
                cc.baseFid = static_cast<RE::FormID>(
                    std::stoul(c.at("baseFid").get<std::string>(), nullptr, 16));
                cc.family = c.at("family").get<std::string>();
                g_calConversions.push_back(std::move(cc));
            }
        }
    } catch (const std::exception& ex) {
        spdlog::error("calibration conversions parse failed ({}): {} — no loot conversion",
                      kPath, ex.what());
        g_calConversions.clear();
    }
    spdlog::info("calibration: {} family recipe(s), {} conversion(s) loaded from {}",
                 g_calibration.size(), g_calConversions.size(), kPath);
}

void ResolveCatalog() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        spdlog::error("TESDataHandler missing — catalog not resolved");
        return;
    }
    g_echoShareSpell = dh->LookupForm<RE::SpellItem>(kEchoShareSpellID, kPluginName);  // m36
    if (!g_echoShareSpell) {
        spdlog::warn("Echo share spell {:X} not found — armor follower-share disabled", kEchoShareSpellID);
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
        rg.mgefLv.fill(rg.mgef);
        if (auto lvIt = g_calLevels.find(def.gid); lvIt != g_calLevels.end() && rg.mgef) {
            std::string ladder;
            for (int l = 0; l < 5; ++l) {
                if (auto* m = dh->LookupForm<RE::EffectSetting>(lvIt->second[l].fid,
                                                               lvIt->second[l].plugin)) {
                    rg.mgefLv[l] = m;
                }
                ladder += std::format("{}{}", l ? "/" : "", rg.mgefLv[l] == rg.mgef ? "-" : "+");
            }
            for (int l = 1; l < 5; ++l) {  // m29: name what each new rung grants
                if (!rg.mgefLv[l] || rg.mgefLv[l] == rg.mgefLv[l - 1]) {
                    continue;
                }
                const char* d = rg.mgefLv[l]->magicItemDescription.c_str();
                if (!d || !*d) {
                    continue;
                }
                std::string s(d);
                if (const auto dot = s.find(". "); dot != std::string::npos) {
                    s = s.substr(dot + 2);  // drop the base stat sentence
                } else {
                    continue;  // single-sentence description: no side grant
                }
                std::erase(s, '<');
                std::erase(s, '>');
                while (!s.empty() && (s.back() == ' ' || s.back() == '.')) {
                    s.pop_back();
                }
                if (!s.empty()) {
                    rg.lvNotes.emplace_back(l + 1, s + ".");
                }
            }
            spdlog::info("[catalog] '{}' rank ladder active ({})", def.gid, ladder);
        }
        if (rg.mgef) {
            const char* live = rg.mgef->GetName();
            if (live && *live) {
                std::string a(live), b(def.name);
                auto lower = [](std::string s) {
                    for (auto& c : s) c = static_cast<char>(std::tolower(c));
                    return s;
                };
                a = lower(a); b = lower(b);
                if (a.find(b) == std::string::npos && b.find(a) == std::string::npos) {
                    rg.liveName = live;
                    spdlog::info("[catalog] '{}' relabeled '{}' — the list renamed its effect",
                                 def.name, live);
                }
            }
        }
        if (!rg.mgef) {
            spdlog::warn("gem '{}' disabled: MGEF {:06X} not found in {}", def.gid, def.mgefID, def.plugin);
        }
        if (auto cal = g_calibration.find(def.gid); cal != g_calibration.end()) {
            for (const auto& cr : cal->second) {
                if (rg.nRiders >= 4) {
                    spdlog::warn("gem '{}': calibration has >4 riders — extras dropped", def.gid);
                    break;
                }
                auto* m = dh->LookupForm<RE::EffectSetting>(cr.fid, cr.plugin);
                if (!m) {
                    spdlog::warn("gem '{}' calibration rider {:06X} not found in {} — skipped",
                                 def.gid, cr.fid, cr.plugin);
                    continue;
                }
                rg.riders[rg.nRiders++] = { m, cr.ratio, cr.absMag, cr.dur };
            }
            if (def.nRiders > 0 && rg.nRiders == 0) {
                spdlog::info("gem '{}': calibration cleared compiled rider default "
                             "(this list's recipe is plain)", def.gid);
            }
        } else {
            for (int r = 0; r < def.nRiders; ++r) {
                auto* m = dh->LookupForm<RE::EffectSetting>(def.riders[r].mgefID,
                                                            def.riders[r].plugin);
                if (!m) {
                    spdlog::warn("gem '{}' rider {:06X} not found in {} — rider skipped",
                                 def.gid, def.riders[r].mgefID, def.riders[r].plugin);
                    continue;
                }
                rg.riders[rg.nRiders++] = { m, def.riders[r].ratio, 0.0f, def.riders[r].duration };
            }
        }
        const int levels = def.singleLevel ? 1 : 5;
        RE::TESObjectMISC* prev = nullptr;
        for (int lv = 0; lv < levels; ++lv) {
            rg.items[lv] = dh->LookupForm<RE::TESObjectMISC>(def.gemItem[lv], kPluginName);
            // Gems are not sellable (marth): they live in the hidden pouch so a
            // vendor never sees them, and a zero gold value closes any transient
            // window where one sits in visible inventory. The ESP bakes 0 too.
            if (rg.items[lv]) {
                rg.items[lv]->value = 0;
            }
            // Short-curve gems (e.g. Muffle) pad higher levels with the top
            // form; map each distinct form to its FIRST level only. m36: support
            // gems have no MGEF (inert alone) but are still real socketable
            // items, so register them by isSupport too.
            if (rg.items[lv] && (rg.mgef || rg.def->isSupport) && rg.items[lv] != prev) {
                g_gemByItem[rg.items[lv]->GetFormID()] = { static_cast<int>(g_gems.size()), lv + 1 };
                if (!rg.liveName.empty()) {  // m27: item names follow the live effect
                    rg.items[lv]->fullName = RE::BSFixedString(
                        (levels == 1) ? std::format("{} Gem", rg.liveName)
                                      : std::format("{} {}", rg.liveName, meo::kRoman[lv]));
                }
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
        if (g_gems[i].def->isSupport) {
            g_supportGems.push_back(static_cast<int>(i));  // m36h: boss-loot pool (tier I)
            continue;
        }
        if (g_gems[i].mgef && !g_gems[i].def->singleLevel) {
            for (int w = 0, n = std::max<int>(g_gems[i].def->spawnWeight, 1); w < n; ++w) {
                g_corpseGems.push_back(static_cast<int>(i));
                if (!g_gems[i].def->isArmor) {
                    g_lootableGems.push_back(static_cast<int>(i));
                }
            }
        }
    }
    // m19: per-archetype themed pools — copies = spawnWeight × theme weight × 4,
    // with S-tier (spawnWeight 1) halved again so rares are rarer on enemies
    // than in world drops (anti-farm, DESIGN §3 post-strip economy).
    for (int a = 0; a < static_cast<int>(Arch::kCount); ++a) {
        for (std::size_t i = 0; i < g_gems.size(); ++i) {
            const auto* def = g_gems[i].def;
            if (!g_gems[i].mgef || def->singleLevel) {
                continue;
            }
            const float tw = kArchThemeW[a][static_cast<int>(def->theme)];
            const float sPenalty = (def->spawnWeight <= 1) ? 0.5f : 1.0f;
            const int copies = static_cast<int>(std::lround(def->spawnWeight * tw * 4.0f * sPenalty));
            for (int w = 0; w < copies; ++w) {
                g_npcPool[a][def->isArmor ? 1 : 0].push_back(static_cast<int>(i));
            }
        }
    }
    g_kwNPC = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeNPC");
    g_kwUndead = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeUndead");
    if (!g_kwNPC) {
        spdlog::warn("[npc] ActorTypeNPC keyword not found — enemy socket spawns disabled");
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
    g_perkTwinned = dh->LookupForm<RE::BGSPerk>(kPerkTwinned, kPluginName);
    g_perkJeweler = dh->LookupForm<RE::BGSPerk>(kPerkJeweler, kPluginName);
    g_perkPyrestone = dh->LookupForm<RE::BGSPerk>(kPerkPyrestone, kPluginName);
    g_perkFroststone = dh->LookupForm<RE::BGSPerk>(kPerkFroststone, kPluginName);
    g_perkStormstone = dh->LookupForm<RE::BGSPerk>(kPerkStormstone, kPluginName);
    g_perkFacet = dh->LookupForm<RE::BGSPerk>(kPerkFacet, kPluginName);
    g_perkArcaneBlacksmith = dh->LookupForm<RE::BGSPerk>(0x05218E, "Skyrim.esm");
    // Tree mode: the installer-generated patch replaces the enchanting perk
    // tree with MEO's perks, so the player earns them with perk points and
    // the interim skill-based auto-grant must stand down.
    g_treeMode = dh->LookupModByName("MEO - Patch.esp") != nullptr;
    if (g_treeMode) {
        spdlog::info("[perks] MEO - Patch.esp present: perk tree mode, auto-grant off");
    }
    // m23: resolve the loot-conversion table against the live load order.
    // NOT LookupForm<TESBoundObject>: that template gates on T::FORMTYPE, and
    // TESBoundObject is an intermediate class with no FORMTYPE of its own
    // (inherits TESForm's FormType::None), so it rejects EVERY real form —
    // shipped as 0.31.0's "0 live, 10146 skipped". Look up as TESForm and
    // As<> down, the cast MenuUnsocket/DestroyGem already prove in the field.
    int convOk = 0, convItemMiss = 0, convBaseMiss = 0, convGemMiss = 0;
    for (const auto& cc : g_calConversions) {
        auto* itemForm = dh->LookupForm(cc.fid, cc.plugin);
        auto* baseForm = dh->LookupForm(cc.baseFid, cc.basePlugin);
        auto* item = itemForm ? itemForm->As<RE::TESBoundObject>() : nullptr;
        auto* base = baseForm ? baseForm->As<RE::TESBoundObject>() : nullptr;
        const auto gid = g_gemByGid.find(cc.family);
        if (!item) { ++convItemMiss; continue; }
        if (!base) { ++convBaseMiss; continue; }
        if (gid == g_gemByGid.end() || !g_gems[gid->second].mgef) { ++convGemMiss; continue; }
        g_convert[item->GetFormID()] = { base, gid->second };
        ++convOk;
    }
    // m35 (audit): drop any row whose TARGET base is itself a conversion
    // source — that would re-arm the arrival sink every pass (item dup / hang).
    int convCyclic = 0;
    for (auto it = g_convert.begin(); it != g_convert.end();) {
        if (it->second.base && g_convert.contains(it->second.base->GetFormID())) {
            it = g_convert.erase(it);
            ++convCyclic;
        } else {
            ++it;
        }
    }
    if (convCyclic > 0) {
        spdlog::warn("[convert] dropped {} cyclic conversion(s) — target was also a source", convCyclic);
    }
    if (convOk + convItemMiss + convBaseMiss + convGemMiss > 0) {
        spdlog::info("[convert] table resolved: {} live, {} skipped (item {}, base {}, gem {})",
                     convOk, convItemMiss + convBaseMiss + convGemMiss,
                     convItemMiss, convBaseMiss, convGemMiss);
    }
    spdlog::info("catalog resolved: {}/{} gems live (weapon+armor), {} socketable gem items, pouch={}, "
                 "mentor={}, soulCairn={}, bossType={}",
                 ok, std::size(meo::kGems), g_gemByItem.size(),
                 g_pouchSpell ? "ok" : "MISSING", g_mentorGem ? "ok" : "MISSING",
                 g_soulCairn ? "ok" : "absent", g_bossRefType ? "ok" : "MISSING");
}

// Sockets an item can hold (m13 multi-socket). Boots are excluded upstream
// (ineligible). Gloves are dual by default; chest needs Twinned Fitting and
// weapons need Master Jeweler (3b-2 socket perks). Everything else is single.
int SocketCapacity(RE::TESBoundObject* a_obj) {
    if (auto* armo = a_obj ? a_obj->As<RE::TESObjectARMO>() : nullptr) {
        if (armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kHands)) {
            return 2;  // gloves
        }
        if (g_hasTwinned && armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
            return 2;  // Twinned Fitting: chest
        }
        return 1;
    }
    if (a_obj && a_obj->Is(RE::FormType::Weapon) && g_hasJeweler) {
        return 2;  // Master Jeweler: weapons
    }
    return 1;
}

// Rebuild an instance's SINGLE combined enchantment from ALL its filled socket
// slots. One created enchant carries one Effect per gem; the name lists them.
// No filled slots -> strip the enchant + name. Caller applies the worn ability.
// This is the multi-socket core: every socket/unsocket/level-up rebuilds here.
// m34 (DESIGN §6): elemental affinities + Facet Insight, each +25%. Chaos is
// every element, so all three affinities stack on it. Facet Insight covers
// skill (SKILL) and attribute (LINEAR) armor gems — not resist/utility.
// m35b: base magnitude — the installer's per-list derived curve if present,
// else the compiled catalog default. All magnitude reads go through here.
float GemBaseMag(const meo::GemDef* a_def, int a_lvIdx) {
    const int li = std::clamp(a_lvIdx, 0, 4);
    if (auto it = g_calCurves.find(a_def->gid); it != g_calCurves.end()) {
        return it->second[li];
    }
    return a_def->magnitude[li];
}

float GemPerkMult(const meo::GemDef* a_def) {
    if (!a_def) {
        return 1.0f;
    }
    float m = 1.0f;
    const bool chaos = std::strcmp(a_def->gid, "chaos") == 0;
    if (g_hasPyrestone && (a_def->theme == meo::Theme::kFire || chaos)) m += 0.25f;
    if (g_hasFroststone && (a_def->theme == meo::Theme::kFrost || chaos)) m += 0.25f;
    if (g_hasStormstone && (a_def->theme == meo::Theme::kShock || chaos)) m += 0.25f;
    if (g_hasFacet && a_def->isArmor &&
        (a_def->gclass == meo::GemClass::kSkill || a_def->gclass == meo::GemClass::kLinear)) {
        m += 0.25f;
    }
    return m;
}

bool IsWornXList(const RE::ExtraDataList* a_xl);  // defined below

// m35c (DESIGN §"Stacking cap"): across ALL worn gear, only the two
// highest-level instances of a given effect (gid) contribute; the 3rd+ copy
// is fully inert. Returns the worn instance-keys that ARE active (top 2 per
// gid). Rebuilds consult this so a single stat caps at 2 x V regardless of
// how many pieces carry it — socket layout adds breadth, not runaway.
std::unordered_set<InstKey> WornActiveEffectKeys() {
    std::unordered_set<InstKey> active;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return active;
    }
    struct Inst { InstKey key; std::string gid; int level; };
    std::unordered_map<std::string, std::vector<Inst>> byGid;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists) {
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
            for (int s = 0; s < kMaxSockets; ++s) {
                const InstKey k = MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                          static_cast<std::uint8_t>(s));
                auto it = g_sockets.find(k);
                if (it != g_sockets.end()) {
                    byGid[it->second.gid].push_back({ k, it->second.gid, it->second.level });
                }
            }
        }
    }
    for (auto& [gid, v] : byGid) {
        std::sort(v.begin(), v.end(), [](const Inst& a, const Inst& b) {
            return a.level != b.level ? a.level > b.level : a.key > b.key;  // stable, level desc
        });
        for (std::size_t i = 0; i < v.size() && i < 2; ++i) {
            active.insert(v[i].key);
        }
    }
    return active;
}

// m35c: how many worn socketed instances share this effect. The cap only
// redistributes (needs a full worn rebuild) when this exceeds 2; otherwise
// the cheap single-item path is correct.
int WornGidCount(std::string_view a_gid) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return 0;
    }
    int n = 0;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists) {
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
            for (int s = 0; s < kMaxSockets; ++s) {
                auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                                 static_cast<std::uint8_t>(s)));
                if (it != g_sockets.end() && it->second.gid == a_gid) {
                    ++n;
                }
            }
        }
    }
    return n;
}

// m36: Focus targets Fire/Frost/Shock/Chaos gems. Theme buckets kFire/kFrost/
// kShock hold both the damage (weapon) and resist (armor) gems of that element;
// Chaos lives in kArcane so it's matched by gid.
bool IsElementalGem(const ResolvedGem* a_rg) {
    if (!a_rg) {
        return false;
    }
    const auto t = a_rg->def->theme;
    return t == meo::Theme::kFire || t == meo::Theme::kFrost || t == meo::Theme::kShock ||
           std::string_view(a_rg->def->gid) == "chaos";
}

// m36: an item is LINKED when exactly one support gem shares it with exactly one
// normal gem (the pairing that lets the support transform the normal, DESIGN §5).
bool ItemIsLinked(RE::FormID a_base, std::uint16_t a_uid) {
    int support = 0, normal = 0;
    for (int s = 0; s < kMaxSockets; ++s) {
        auto it = g_sockets.find(MakeKey(a_base, a_uid, static_cast<std::uint8_t>(s)));
        if (it == g_sockets.end()) {
            continue;
        }
        auto gi = g_gemByGid.find(it->second.gid);
        if (gi == g_gemByGid.end()) {
            continue;
        }
        (g_gems[gi->second].def->isSupport ? support : normal)++;
    }
    return support == 1 && normal == 1;
}

// m36: does this item instance already hold a Conduit? A Conduit is the adapter
// that lets its item accept an OFF-domain normal gem (DESIGN §5).
bool ItemHasConduit(RE::FormID a_base, std::uint16_t a_uid) {
    for (int s = 0; s < kMaxSockets; ++s) {
        auto it = g_sockets.find(MakeKey(a_base, a_uid, static_cast<std::uint8_t>(s)));
        if (it == g_sockets.end()) {
            continue;
        }
        auto gi = g_gemByGid.find(it->second.gid);
        if (gi != g_gemByGid.end() && g_gems[gi->second].def->isSupport &&
            g_gems[gi->second].def->supportType == meo::SupportType::kConduit) {
            return true;
        }
    }
    return false;
}

// m36: Conduit maps a linked OFF-domain gem onto the catalog's same-theme
// sibling of the ITEM's domain, and applies that sibling's own per-list
// calibrated effect (a Fire Damage weapon gem in armor -> the Resist Fire armor
// gem). Reusing the sibling's curve keeps units honest (no damage-vs-resist
// transfer). nullptr = no clean mapping for this theme -> the gem stays inert.
const ResolvedGem* ConduitSibling(const ResolvedGem* a_normal, bool a_itemIsArmor) {
    if (!a_normal) {
        return nullptr;
    }
    for (const auto& g : g_gems) {
        if (g.def->isSupport || !g.mgef) {
            continue;
        }
        if (g.def->isArmor == a_itemIsArmor && g.def->theme == a_normal->def->theme) {
            return &g;
        }
    }
    return nullptr;
}

void RebuildInstanceEnchant(RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList) {
    auto* xid = a_xList ? a_xList->GetByType<RE::ExtraUniqueID>() : nullptr;
    if (!a_base || !xid) {
        return;
    }
    const std::uint16_t uid = xid->uniqueID;
    const bool          isArmor = a_base->Is(RE::FormType::Armor);

    struct Filled { const ResolvedGem* rg; int lvIdx; };
    // m35c: worn items enforce the 2-of-a-kind stacking cap; a 3rd+ copy of
    // an effect across worn gear contributes nothing.
    const bool worn = IsWornXList(a_xList);
    const std::unordered_set<InstKey> active = worn ? WornActiveEffectKeys()
                                                    : std::unordered_set<InstKey>{};
    std::vector<Filled> filled;
    // m36: a support gem (Echo/Conduit/Focus) has no effect of its own. When
    // exactly ONE support shares the item with exactly ONE normal gem it is
    // LINKED and transforms that normal gem; a lone support, or two supports,
    // is inert. Supports are collected apart from the effect-producing normals.
    const ResolvedGem* support = nullptr;
    int supportCount = 0;
    int supportTier = 1;
    for (int slot = 0; slot < kMaxSockets; ++slot) {
        const InstKey key = MakeKey(a_base->GetFormID(), uid, static_cast<std::uint8_t>(slot));
        auto it = g_sockets.find(key);
        if (it == g_sockets.end()) {
            continue;
        }
        auto gemIt = g_gemByGid.find(it->second.gid);
        if (gemIt == g_gemByGid.end()) {
            continue;
        }
        const ResolvedGem* rg = &g_gems[gemIt->second];
        if (rg->def->isSupport) {
            ++supportCount;
            support = rg;
            supportTier = std::clamp<int>(it->second.level, 1, 3);
            continue;  // no direct effect
        }
        if (!rg->mgef) {
            continue;  // disabled normal gem (missing master)
        }
        if (worn && !active.contains(key)) {
            continue;  // m35c: capped 3rd+ same-effect copy — inert
        }
        filled.push_back({ rg, std::clamp<int>(it->second.level, 1, 5) - 1 });
    }
    const bool linked = (supportCount == 1 && filled.size() == 1);
    if (!linked) {
        support = nullptr;  // inert unless it's a clean 1-support + 1-normal pair
    }
    // m36: an OFF-domain normal gem only has an effect through a linked Conduit
    // adapting it. Without that (Conduit removed, or not linked) it must stay
    // inert — never apply its native cross-domain effect (a weapon damage MGEF
    // riding armor could damage the wearer).
    const bool conduitLinked =
        support && support->def->supportType == meo::SupportType::kConduit;
    if (!conduitLinked) {
        std::erase_if(filled, [&](const Filled& f) { return f.rg->def->isArmor != isArmor; });
    }
    if (filled.empty()) {  // no normal gem left — return the item to plain (a
        a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);      // lone or
        a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);  // double
        return;                                                     // support is inert
    }
    // m36: Focus lifts a linked elemental gem's magnitude (+20/35/50% by tier).
    float supportMag = 1.0f;
    if (support && support->def->supportType == meo::SupportType::kFocus &&
        IsElementalGem(filled[0].rg)) {
        supportMag = 1.0f + support->def->tierParam[supportTier - 1];
    }
    // m36: Conduit adapts a linked OFF-domain gem — its effect is replaced by the
    // same-theme sibling of the ITEM's domain, at that sibling's own calibrated
    // magnitude × Conduit's transfer ratio (tierParam). A same-domain gem passes
    // through unchanged; an off-domain gem with no clean sibling is inert.
    const ResolvedGem* conduitTarget = nullptr;
    float              conduitRatio = 1.0f;
    if (support && support->def->supportType == meo::SupportType::kConduit &&
        filled[0].rg->def->isArmor != isArmor) {
        conduitTarget = ConduitSibling(filled[0].rg, isArmor);
        conduitRatio = support->def->tierParam[supportTier - 1];
        if (!conduitTarget) {  // off-domain gem, no mapping for this theme — inert
            a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
            a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);
            return;
        }
    }
    // m36: Echo on a WEAPON gives the linked on-hit effect an area (AoE) that
    // grows with tier. Start with the obvious damaging elemental gems (marth);
    // more gems opt in as their linkage is added. (Armor Echo = follower-share,
    // a runtime heartbeat handled separately.)
    int echoArea = 0;
    if (support && support->def->supportType == meo::SupportType::kEcho &&
        !isArmor && IsElementalGem(filled[0].rg)) {
        echoArea = static_cast<int>(std::lround(support->def->tierParam[supportTier - 1] * 15.0f));
    }
    // m36: spell out the linkage so "kind of works" reports have a paper trail.
    if (support) {
        const char* type = support->def->supportType == meo::SupportType::kFocus     ? "Focus"
                           : support->def->supportType == meo::SupportType::kConduit  ? "Conduit"
                           : support->def->supportType == meo::SupportType::kEcho     ? "Echo"
                                                                                      : "?";
        spdlog::info("[link] {:08X}/{} {} T{} + '{}' L{} ({}): focusMag={:.2f} conduit->{} echoArea={} "
                     "elemental={}",
                     a_base->GetFormID(), uid, type, supportTier, filled[0].rg->def->gid,
                     filled[0].lvIdx + 1, isArmor ? "armor" : "weapon", supportMag,
                     conduitTarget ? conduitTarget->def->gid : "-", echoArea,
                     IsElementalGem(filled[0].rg));
    } else if (supportCount > 0) {
        spdlog::info("[link] {:08X}/{} has {} support(s) but is NOT linked (need exactly 1 support "
                     "+ 1 normal) — support inert", a_base->GetFormID(), uid, supportCount);
    }

    // One primary effect per filled socket, plus that gem's recipe riders
    // (m21, marth: gems mirror the load order's elemental recipe — frost
    // carries slow, shock carries magicka bite — at ratio × primary).
    // m36: when Conduit remaps, the single effect is the sibling's (no riders).
    std::size_t nEff = 0;
    for (const auto& f : filled) {
        nEff += 1;
        if (!conduitTarget) {
            for (int r = 0; r < f.rg->nRiders; ++r) {
                nEff += f.rg->riders[r].mgef != nullptr;
            }
        }
    }
    RE::BSTArray<RE::Effect> effects;
    effects.resize(nEff);
    std::string namePart;
    std::size_t e = 0;
    for (std::size_t i = 0; i < filled.size(); ++i) {
        // m36: Conduit substitutes the item-domain sibling gem for effect
        // purposes; the normal gem's LEVEL still drives the tier.
        const ResolvedGem* eg = conduitTarget ? conduitTarget : filled[i].rg;
        const int          lvIdx = filled[i].lvIdx;
        // Master power scale (MCM) × Gem Attunement (+8%/rank) × affinity/
        // Facet Insight (+25% each, DESIGN §6); × Focus boost or Conduit ratio.
        const float baseMag =
            GemBaseMag(eg->def, lvIdx) * g_magnitudeMult *
            (1.0f + 0.08f * g_attuneRank) * GemPerkMult(eg->def);
        const float primaryMag = conduitTarget ? baseMag * conduitRatio : baseMag * supportMag;
        auto& eff = effects[e++];
        eff.effectItem.magnitude = primaryMag;
        eff.effectItem.area = echoArea;  // m36: Echo weapon AoE (0 otherwise)
        eff.effectItem.duration = static_cast<std::uint32_t>(eg->def->duration);
        eff.baseEffect = eg->mgefLv[lvIdx];  // m28: rank ladder
        eff.cost = 0.0f;
        if (!conduitTarget) {
            for (int r = 0; r < filled[i].rg->nRiders; ++r) {
                const auto& rd = filled[i].rg->riders[r];
                if (!rd.mgef) {
                    continue;
                }
                auto& reff = effects[e++];
                reff.effectItem.magnitude =
                    rd.absMag > 0.0f
                        ? rd.absMag * g_magnitudeMult * (1.0f + 0.08f * g_attuneRank) *
                              GemPerkMult(filled[i].rg->def) * supportMag  // m32/m34; m36 Focus

                        : primaryMag * rd.ratio;
                reff.effectItem.area = echoArea;  // m36: rider shares the AoE
                reff.effectItem.duration = static_cast<std::uint32_t>(rd.dur);
                reff.baseEffect = rd.mgef;
                reff.cost = 0.0f;
            }
        }
        if (!namePart.empty()) {
            namePart += " + ";
        }
        namePart += std::format("{} {}", GemName(*eg), meo::kRoman[lvIdx]);
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
    // m36: a linked support prefixes the name so the pairing is legible on the
    // item itself, e.g. "Focus III · Fire II Sword".
    if (support) {
        namePart = std::format("{} {} \xC2\xB7 {}", GemName(*support),
                               meo::kRoman[supportTier - 1], namePart);
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
    std::string mags;
    for (std::size_t i = 0; i < effects.size(); ++i) {
        mags += std::format("{}{:.1f}", i ? "/" : "", effects[i].effectItem.magnitude);
    }
    spdlog::info("[rebuild] {:08X}/{}: '{}' ({} gem(s), mag {})", a_base->GetFormID(), uid,
                 newName, filled.size(), mags);
}

// Socket one gem into slot a_slot, then rebuild the instance's combined enchant.
// Returns the uid (minted if absent), or 0 on failure. Caller does the worn
// ability + gem consumption. a_xp carries banked XP through a level-up re-stamp.
std::uint16_t StampInstance(RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                            int a_gemIdx, int a_level, std::uint8_t a_slot = 0, float a_xp = 0.0f) {
    if (a_gemIdx < 0 || !a_base || !a_xList ||
        (!g_gems[a_gemIdx].mgef && !g_gems[a_gemIdx].def->isSupport)) {  // m36: supports have no mgef
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
    if (!a_weap || a_weap->formEnchanting || !a_weap->GetPlayable() ||
        a_weap->IsHandToHandMelee() || a_weap->IsBound() ||
        a_weap->HasKeywordString("MagicDisallowEnchanting")) {
        return false;
    }
    // m19d (marth: "no socketed pickaxes"): tools and nameless bases are out.
    // Byte-diffed Skyrim.esm — vanilla gives tools NO semantic marker (same
    // animType/skill as war axes; the plain Pickaxe even carries
    // WeapTypeWarAxe; Woodcutter's Axe has no WeapType* at all), so a name
    // blocklist is the honest option.
    const char* n = a_weap->GetName();
    if (!n || !*n) {
        return false;  // nameless pseudo-weapons (trap/effect bases)
    }
    static constexpr const char* kToolWords[] = { "Pickaxe", "Pick Axe", "Woodcutter",
                                                  "Wood Axe", "Woodsman" };
    for (const char* w : kToolWords) {
        if (std::strstr(n, w)) {
            return false;
        }
    }
    return true;
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
    // kHair: vanilla-line helmets occupy biped slot 31 (Hair), not 30 —
    // head gear was silently ineligible until 2026-07-10 (marth's helmet
    // "became unslotted" once its conversion record was removed).
    // kCirclet (slot 42): circlets occupy their own slot, not kHead — without
    // this they were hidden from the socketing menu, refused instance conversion
    // ("not socketable gear"), and never stamped on NPCs, even though the
    // installer's table already converts base-enchanted circlets (m36f, Fable).
    return a_armo->HasPartOf(S::kHead) || a_armo->HasPartOf(S::kHair) ||
           a_armo->HasPartOf(S::kBody) || a_armo->HasPartOf(S::kHands) ||
           a_armo->HasPartOf(S::kAmulet) || a_armo->HasPartOf(S::kRing) ||
           a_armo->HasPartOf(S::kCirclet);
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

// m23c (marth's stale-Resist-Fire report): Update*Ability REPLACES a worn
// ability when an enchant extra is present, but when the extra is GONE it
// early-outs without unregistering — removing the LAST gem from a worn item
// left its constant effect active until a real unequip. The engine's own
// complete teardown is the equip cycle (m16-proven, m17b field-validated),
// and behind the open menu its one-frame blink is invisible.
void EquipCycleWorn(RE::Actor* a_owner, RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList) {
    auto* em = RE::ActorEquipManager::GetSingleton();
    if (!em || !a_owner || !a_base || !a_xList) {
        return;
    }
    const RE::BGSEquipSlot* slot = nullptr;
    if (a_base->Is(RE::FormType::Weapon)) {
        if (auto* dom = RE::BGSDefaultObjectManager::GetSingleton()) {
            slot = dom->GetObject<RE::BGSEquipSlot>(
                a_xList->HasType(RE::ExtraDataType::kWornLeft)
                    ? RE::DEFAULT_OBJECT::kLeftHandEquip
                    : RE::DEFAULT_OBJECT::kRightHandEquip);
        }
    }
    em->UnequipObject(a_owner, a_base, a_xList, 1, slot, false, false, false, true);
    em->EquipObject(a_owner, a_base, a_xList, 1, slot, false, false, false, true);
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
    const int topLevel = a_def->isSupport ? 3 : 5;  // m36: supports master at III
    return (a_level >= 1 && a_level < topLevel) ? meo::kXPThresholds[a_level - 1] * a_def->xpMult
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
float g_npcSocketChance = 0.05f;  // [Loot] fNPCSocketChance — enemy spawns with a socketed piece (m19)
float g_vendorGemChance = 0.04f;  // [Loot] fVendorGemChance — per stock item, vendor adds a gem (m19b)
float g_supportDropChance = 0.03f;// [Loot] fSupportDropChance — support gem on a lvl15+ boss/dragon kill (m36h, very rare)
int   g_supportMinLevel = 15;     // [Loot] iSupportMinLevel — no support gems before this player level
float g_bossXPMult = 10.0f;       // [XP] fBossXPMult — boss/dragon kill multiplier
bool  g_xpNotify = true;          // [UI] bXPNotify — "Gem XP +N" on kills
bool  g_stationTakeover = true;   // [UI] bStationTakeover — gem menu REPLACES the vanilla enchanting menu
int   g_menuStyle = 0;            // [UI] iMenuStyle — gem menu skin 0..3 (m24 MCM dropdown)
bool  g_temperNoPerk = true;      // [UI] bTemperNoPerk — socketed gear tempers w/o Arcane Blacksmith (m33)
float g_enchSkillXPMult = 1.0f;   // [XP] fEnchSkillXP — Enchanting SKILL xp per soul fed (m25)
float g_discoverSkillXP = 50.0f;  // [XP] fDiscoverSkillXP — Enchanting SKILL xp for discovering a NEW gem family (one-time each, m37)
float g_destroySkillXP  = 20.0f;  // [XP] fDestroySkillXP — Enchanting SKILL xp per gem destroyed (× level) (m37)
float g_levelSkillXP    = 12.0f;  // [XP] fLevelSkillXP — Enchanting SKILL xp per gem level gained (× new level) (m37)
float g_gemXpSkillXP    = 0.05f;  // [XP] fGemXpSkillXP — Enchanting SKILL xp per point of Gem XP a gem earns from a kill (tiny trickle, m37)
// NB: socketing grants NO skill xp on purpose — socket/unsocket would be a farm loop (marth).
bool  g_debugAllPerks = false;    // [Debug] bDebugAllPerks — force every MEO perk ON for testing (m36)
bool  g_purgeSupportGems = false; // [Debug] bPurgeSupportGems — one-shot: strip all support gems from inv+pouch (m36l cleanup)
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
        else if (key == "fNPCSocketChance")   g_npcSocketChance = val;
        else if (key == "fSupportDropChance")  g_supportDropChance = val;
        else if (key == "iSupportMinLevel")    g_supportMinLevel = static_cast<int>(val);
        else if (key == "fVendorGemChance")   g_vendorGemChance = val;
        else if (key == "fBossXPMult")        g_bossXPMult = val;
        else if (key == "fMagnitudeMult")     g_magnitudeMult = val;
        else if (key == "bXPNotify")          g_xpNotify = val != 0.0f;
        else if (key == "bStationTakeover")   g_stationTakeover = val != 0.0f;
        else if (key == "iMenuStyle")         g_menuStyle = std::clamp(static_cast<int>(val), 0, 3);
        else if (key == "bTemperNoPerk")      g_temperNoPerk = val != 0.0f;
        else if (key == "fEnchSkillXP")       g_enchSkillXPMult = val;
        else if (key == "fDiscoverSkillXP")   g_discoverSkillXP = val;
        else if (key == "fDestroySkillXP")    g_destroySkillXP = val;
        else if (key == "fLevelSkillXP")      g_levelSkillXP = val;
        else if (key == "fGemXpSkillXP")      g_gemXpSkillXP = val;
        else if (key == "bDebugAllPerks")     g_debugAllPerks = val != 0.0f;
        else if (key == "bPurgeSupportGems")  g_purgeSupportGems = val != 0.0f;
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
    spdlog::info("config: drop={:.3f} world={:.3f} lvl2={:.3f} xp={:.2f} boss={:.1f} mag={:.2f} notify={} skin={}",
                 g_gemDropChance, g_worldSocketChance, g_gemLevel2Chance, g_xpPerKill,
                 g_bossXPMult, g_magnitudeMult, g_xpNotify, g_menuStyle);
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
    if (!g_treeMode) {
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
        grant(g_perkTwinned, skill >= 70.0f);   // DESIGN §6 Corpus-tier req
        grant(g_perkJeweler, skill >= 100.0f);  // DESIGN §6 Extra Effect req
        grant(g_perkPyrestone, skill >= 30.0f);   // Fire Enchanter req (m34)
        grant(g_perkFroststone, skill >= 40.0f);  // Frost Enchanter req
        grant(g_perkStormstone, skill >= 50.0f);  // Storm Enchanter req
        grant(g_perkFacet, skill >= 50.0f);       // Insightful Enchanter req
    }
    g_attuneRank = 0;
    for (int i = 0; i < 5; ++i) {
        if (g_perkAttune[i] && player->HasPerk(g_perkAttune[i])) {
            ++g_attuneRank;
        }
    }
    g_hasGemCutter = g_perkGemCutter && player->HasPerk(g_perkGemCutter);
    g_hasSoulFeeder = g_perkSoulFeeder && player->HasPerk(g_perkSoulFeeder);
    g_hasTwinned = g_perkTwinned && player->HasPerk(g_perkTwinned);
    g_hasJeweler = g_perkJeweler && player->HasPerk(g_perkJeweler);
    g_hasPyrestone = g_perkPyrestone && player->HasPerk(g_perkPyrestone);
    g_hasFroststone = g_perkFroststone && player->HasPerk(g_perkFroststone);
    g_hasStormstone = g_perkStormstone && player->HasPerk(g_perkStormstone);
    g_hasFacet = g_perkFacet && player->HasPerk(g_perkFacet);
    // m36 debug (marth): force every MEO perk ON for testing without grinding
    // Enchanting or spending points. Overrides only the CACHED flags — no perks
    // are added to the player, so toggling it off (menu close → RefreshPerks)
    // reverts to what's actually held. Grants both dual-socket perks (Twinned +
    // Master Jeweler) so chest and weapons become dual/linkable too.
    if (g_debugAllPerks) {
        g_attuneRank = 5;
        g_hasGemCutter = g_hasSoulFeeder = g_hasTwinned = g_hasJeweler = true;
        g_hasPyrestone = g_hasFroststone = g_hasStormstone = g_hasFacet = true;
        spdlog::warn("[perks] DEBUG bDebugAllPerks=ON — all MEO perks forced (attune 5, "
                     "both socket perks, affinities, facet, cutter, feeder)");
    }
    spdlog::info("[perks] enchanting={:.0f} treeMode={} attuneRank={} gemCutter={} soulFeeder={} twinned={} jeweler={} pyre={} frost={} storm={} facet={}",
                 skill, g_treeMode, g_attuneRank, g_hasGemCutter, g_hasSoulFeeder, g_hasTwinned, g_hasJeweler,
                 g_hasPyrestone, g_hasFroststone, g_hasStormstone, g_hasFacet);
}

// Menu snapshot rows + shared state (declared here so MenuSink can read
// g_menu.station; the menu is built/drawn much further down).
struct MenuItemRow {
    std::string   label;
    RE::FormID    base = 0;
    std::uint16_t uid = 0;       // 0 = plain stack (xList materialized on socket)
    bool          worn = false;
    bool          isArmor = false;    // gates which gems can socket into it
    bool          hasConduit = false; // m36: a Conduit here adapts off-domain gems
    int           capacity = 1;       // socket slots this item has (1 or 2)
    std::string   slotGem[2];         // per-slot gem label; empty = empty slot
    int           slotGemIdx[2] = { -1, -1 };  // m29: rung notes in tooltips
    int           slotLevel[2] = { 0, 0 };
    int           slotTheme[2] = { -1, -1 };  // Theme index for the accent swatch
    float         slotXp[2] = { 0.0f, 0.0f };
    float         slotNeed[2] = { 0.0f, 0.0f };  // 0 = mastered / never levels
};
struct MenuSoulRow {
    std::string label;   // "Grand Soul Gem  x3"
    RE::FormID  base = 0;
    int         soul = 1;   // contained soul 1..5 — the feed tier
};
struct MenuGemRow {
    std::string   label;
    RE::FormID    base = 0;
    std::uint16_t uid = 0;       // instance with banked XP, else 0
    bool          isArmor = false;
    bool          isSupport = false;  // m36: support gem — fits any dual-socket item
    int           gemIdx = -1;   // m29: rung notes in tooltips
    int           level = 1;
    int           theme = -1;    // Theme index for the accent swatch
    float         xp = -1.0f;    // banked XP (instance rows); -1 = plain gem
    float         need = 0.0f;
};
struct MenuState {
    std::mutex               lock;
    std::atomic<bool>        open{ false };
    std::atomic<bool>        busy{ false };      // an action task is in flight
    std::atomic<bool>        wantClose{ false }; // set by input, consumed by draw
    std::atomic<bool>        station{ false };   // opened at an enchanting station (feed/destroy)
    std::vector<MenuItemRow> items;
    std::vector<MenuGemRow>  gems;
    std::vector<MenuSoulRow> souls;   // m25 station redesign: feed fuel list
    int                      selItem = 0;
    int                      selSlot = -1;  // m25: station mode's selected socket
    // m19e: selection is IDENTITY, not index — rows are label-sorted and a
    // socket/unsocket changes the label, moving the row. After each rebuild
    // the index is re-derived from (base, uid); a raw index silently landed
    // on a DIFFERENT item (marth: "frost disappeared", phantom-empty sockets).
    RE::FormID               selBase = 0;
    std::uint16_t            selUid = 0;
};
MenuState g_menu;

void OpenGemMenu(bool a_station = false);  // defined with the render hooks below
void ApplyTemperPerk();                    // m33b — defined before EnsurePlayerSetup
void DispelStaleGemEffects();              // m24b/c — defined with the load-refresh code
void StockVendorGems();                    // m19b — defined with the loot rolls below
int  ConvertInventory(RE::TESObjectREFR* a_holder);  // m38 — defined with conversion below
void CloseGemMenu();
extern std::atomic<bool> g_pendingReapply;  // m19e — defined with the load reapply below
void RunDeferredReapply(int a_delayMs);
void ReapplyWornSockets(bool a_rebuild, bool a_reequip, bool a_diag);  // m35c cap redistribution

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
            ApplyTemperPerk();  // m33: MCM toggle takes effect on menu close
        } else if (!a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
            // m19e: gameplay just resumed after a load — NOW the worn-socket
            // re-equip can take (a blind post-load timer fired during long
            // loading screens and was swallowed; see ScheduleReapplyWornSockets).
            if (g_pendingReapply.exchange(false)) {
                // Two passes: the refresh is blinkless (strip/restamp, no
                // equip cycle), so a late second pass is free insurance —
                // +1.5s was field-swallowed during the fade (2026-07-09).
                RunDeferredReapply(5000);
                RunDeferredReapply(12000);
            }
        } else if (a_event->opening && a_event->menuName == RE::DialogueMenu::MENU_NAME) {
            // m19e: stock at DIALOGUE open — stocking at BarterMenu open
            // mutated the merchant chest while the barter UI was building
            // its item list and broke it (Belethor, 2026-07-09). By the time
            // the player picks the trade line, stock is settled.
            SKSE::GetTaskInterface()->AddTask([]() { StockVendorGems(); });
        } else if (a_event->opening && a_event->menuName == RE::BarterMenu::MENU_NAME) {
            // m38: the engine restocks the vendor from its leveled lists as the
            // BARTER menu opens — i.e. AFTER the m19e dialogue-open sweep — so the
            // freshly re-rolled enchanted generics land unconverted (deck test
            // 2026-07-13: the chest converted fine at dialogue time, then restock
            // dropped "of Burning"/"Minor Archery"/"Minor Alteration"/... back on
            // top). Sweep again here. Defer two frames so the barter list is fully
            // built before we mutate the chest (mutating mid-build is exactly the
            // m19e breakage), then let the engine rebuild the list via its own
            // inventory-update signal — the same one a buy/sell emits.
            SKSE::GetTaskInterface()->AddTask([]() {
                SKSE::GetTaskInterface()->AddTask([]() {
                    auto* mtm = RE::MenuTopicManager::GetSingleton();
                    auto  speaker = mtm ? mtm->speaker.get()
                                        : RE::NiPointer<RE::TESObjectREFR>{};
                    auto* vendor = speaker ? speaker->As<RE::Actor>() : nullptr;
                    if (!vendor) {
                        return;
                    }
                    RE::TESObjectREFR* target = vendor;  // fallback: sells own inventory
                    if (auto* base = vendor->GetActorBase()) {
                        for (auto& fr : base->factions) {
                            if (fr.faction && fr.faction->IsVendor() &&
                                fr.faction->vendorData.merchantContainer) {
                                target = fr.faction->vendorData.merchantContainer;
                                break;
                            }
                        }
                    }
                    const int n = ConvertInventory(target);
                    if (n > 0) {
                        // Rebuild the open barter list from the now-converted stock
                        // by calling the game's OWN inventory-update routine — the
                        // same one a buy/sell fires (call the engine's function, as
                        // SKSE does — never hand-repaint the UI). CommonLibSSE-NG
                        // 3.7.0 predates RE::SendUIMessage::SendInventoryUpdateMessage,
                        // so bind the relocation it uses (SE 51911 / AE 52849).
                        static REL::Relocation<void(RE::TESObjectREFR*,
                                                    const RE::TESBoundObject*)>
                            sendInvUpdate{ RELOCATION_ID(51911, 52849) };
                        sendInvUpdate(target, nullptr);
                        spdlog::info("[vendor] barter restock sweep: {} converted, "
                                     "list refreshed", n);
                    }
                });
            });
        } else if (!a_event->opening && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
            // Overlay mode only: leaving the vanilla enchanting UI takes the gem
            // menu with it. In takeover mode (m18) the crafting menu closing IS
            // our own dismissal — the gem menu stands alone and closes itself.
            if (g_menu.station.load() && !g_stationTakeover) {
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
                    if (g_stationTakeover) {
                        // MEO replaces enchanting: dismiss the vanilla menu
                        // and let the gem menu own the station (m18).
                        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                            q->AddMessage(RE::CraftingMenu::MENU_NAME,
                                          RE::UI_MESSAGE_TYPE::kHide, nullptr);
                        }
                    }
                    OpenGemMenu(true);
                }
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

bool IsWornXList(const RE::ExtraDataList* a_xl);  // defined below

// ── m37: Enchanting SKILL xp from regular play (not just soul feeding) ──
// Non-abusable sources (marth): DISCOVER a new gem family (one-time), DESTROY a
// gem, gem LEVEL-UPS (from kills), and a tiny per-kill TRICKLE proportional to
// Gem XP earned. Socketing grants NOTHING (socket/unsocket would be a farm loop).
void GrantEnchantingXP(float a_xp) {
    if (a_xp <= 0.0f || g_enchSkillXPMult <= 0.0f) {
        return;
    }
    if (auto* p = RE::PlayerCharacter::GetSingleton()) {
        p->AddSkillExperience(RE::ActorValue::kEnchanting, a_xp * g_enchSkillXPMult);
    }
}

// First time the player possesses a gem of a family, they "study" it: one-time
// Enchanting xp + notify, persisted in g_discoveredGems (co-save v11).
void DiscoverGem(const ResolvedGem& a_rg) {
    std::string gid(a_rg.def->gid);
    if (g_discoveredGems.count(gid)) {
        return;
    }
    g_discoveredGems.insert(gid);
    GrantEnchantingXP(g_discoverSkillXP);
    Notify(std::format("You study a new gem: {}. (Enchanting rises.)", GemName(a_rg)));
    spdlog::info("[discover] '{}' — +{:.0f} Enchanting xp", gid, g_discoverSkillXP * g_enchSkillXPMult);
}

// Scan the player's inventory + pouch and discover any new gem family.
void CheckGemDiscoveries() {
    auto scan = [](RE::TESObjectREFR* a_h) {
        if (!a_h) {
            return;
        }
        for (const auto& [obj, data] : a_h->GetInventory(
                 [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
            if (data.first <= 0) {
                continue;
            }
            auto it = g_gemByItem.find(obj->GetFormID());
            if (it != g_gemByItem.end()) {
                DiscoverGem(g_gems[it->second.first]);
            }
        }
    };
    scan(RE::PlayerCharacter::GetSingleton());
    scan(PouchRef());
}

// Pre-v11 save upgrade: mark every gem family already held or socketed as
// discovered WITHOUT xp/notify, so only genuinely-new finds grant afterward.
void SeedDiscoveries() {
    auto seed = [](RE::TESObjectREFR* a_h) {
        if (!a_h) {
            return;
        }
        for (const auto& [obj, data] : a_h->GetInventory(
                 [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
            if (data.first > 0) {
                auto it = g_gemByItem.find(obj->GetFormID());
                if (it != g_gemByItem.end()) {
                    g_discoveredGems.insert(std::string(g_gems[it->second.first].def->gid));
                }
            }
        }
    };
    seed(RE::PlayerCharacter::GetSingleton());
    seed(PouchRef());
    for (const auto& [key, rec] : g_sockets) {
        g_discoveredGems.insert(rec.gid);
    }
    spdlog::info("[discover] seeded {} already-known gem families (pre-v11 save)",
                 g_discoveredGems.size());
}

// Add Gem XP to one socketed worn instance; handles the level-up re-stamp,
// notifications (named for followers), and mastered births. a_rec must be
// the live g_sockets entry (StampInstance rewrites the same key — the
// reference stays valid).
bool GrantGemXP(RE::Actor* a_owner, RE::TESBoundObject* a_base, RE::ExtraDataList* a_xList,
                bool a_left, SocketRecord& a_rec, int a_gemIdx, float a_xp,
                std::uint16_t a_uid) {
    const auto& rg = g_gems[a_gemIdx];
    const bool  isSupport = rg.def->isSupport;
    const int   maxLevel = isSupport ? 3 : 5;  // m36: supports cap at tier III
    if ((!rg.mgef && !isSupport) || rg.def->xpMult <= 0.0f || a_rec.level >= maxLevel) {
        return false;  // single-level / disabled / mastered gems never level
    }
    // m36: a support gem earns XP only while LINKED to a working normal gem; an
    // inert, unlinked support gains nothing (DESIGN §5).
    if (isSupport && !ItemIsLinked(a_base->GetFormID(), a_uid)) {
        return false;
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
        // m35c: a level-up shifts cap ranking; redistribute across worn gear
        // only when the effect actually has >2 worn copies (player only).
        if (a_owner->IsPlayerRef() && WornGidCount(a_rec.gid) > 2) {
            ReapplyWornSockets(true, true, false);
        } else {
            ApplyWornAbility(a_owner, a_base, a_xList, a_left);
        }
        if (a_owner->IsPlayerRef()) {
            DispelStaleGemEffects();  // m24c: replace can leave the old-level ability stacking
        }
    }
    const bool isPlayer = a_owner->IsPlayerRef();
    if (isPlayer) {  // m37: a gem reaching a new level is Enchanting practice (× the level)
        GrantEnchantingXP(g_levelSkillXP * newLevel);
    }
    const char* who = a_owner->GetName();
    Notify(isPlayer
               ? std::format("Your {} gem has grown to {}.", GemName(rg), meo::kRoman[newLevel - 1])
               : std::format("{}'s {} gem has grown to {}.", who && *who ? who : "Your follower",
                             GemName(rg), meo::kRoman[newLevel - 1]));
    if (isPlayer) {  // m29: crossing a rank rung announces what it grants
        for (const auto& [lv, note] : rg.lvNotes) {
            if (lv == newLevel) {
                Notify(std::format("Rank attained — {}", note));
            }
        }
    }
    if (newLevel == 5 && rg.items[0]) {
        a_owner->AddObjectToContainer(rg.items[0], nullptr, 1, nullptr);
        Notify(isPlayer ? std::format("Your mastered {} gem births a new gem.", GemName(rg))
                        : std::format("{}'s mastered {} gem births a new gem.",
                                      who && *who ? who : "Your follower", GemName(rg)));
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
    // m37: tiny Enchanting trickle for the player — the Gem XP a kill puts into
    // your gems is enchanting practice. Once per kill (not × gem count, so more
    // sockets isn't a faster grind); accumulates over normal combat.
    if (awarded > 0 && a_owner->IsPlayerRef()) {
        GrantEnchantingXP(xp * g_gemXpSkillXP);
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
        auto* pouch = PouchRef();  // m27: no bag clutter
        (pouch ? pouch : static_cast<RE::TESObjectREFR*>(player))
            ->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
        return;
    }
    auto ref = player->PlaceObjectAtMe(gemForm, false);
    if (!ref) {
        spdlog::error("[menu] PlaceObjectAtMe failed for '{}' — plain gem given instead", rg.def->gid);
        player->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
        return;
    }
    auto&               xl = ref->extraList;
    // A PlaceObjectAtMe ref has NO owner, so ownership falls back to the
    // cell/location owner — picking it up in town, witnessed, was THEFT
    // (marth's 100g bounty on gem swaps near guards, m17b). Own it first.
    xl.SetOwner(player->GetActorBase());
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
    std::vector<MenuSoulRow> souls;
    // m27: one collector, two containers — gems normally live in the
    // pouch, but anything still in transit through the player's bags
    // (route task pending) is listed too so nothing ever vanishes.
    auto collectGemRows = [&](RE::TESBoundObject* obj, std::int32_t a_total,
                              RE::InventoryEntryData* a_entry) {
                auto it = g_gemByItem.find(obj->GetFormID());
                if (it == g_gemByItem.end()) {
                    return;
                }
                std::int32_t plain = a_total;
                if (a_entry && a_entry->extraLists) {
                    for (auto* xl : *a_entry->extraLists) {
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
                        row.isSupport = rg.def->isSupport;
                        row.uid = xid->uniqueID;
                        row.label = obj->GetName();
                        row.gemIdx = it->second.first;
                        row.level = recIt->second.level;
                        row.theme = static_cast<int>(rg.def->theme);
                        row.xp = recIt->second.xp;
                        row.need = NextThreshold(rg.def, recIt->second.level);
                        gems.push_back(std::move(row));
                    }
                }
                if (plain > 0) {
                    MenuGemRow row;
                    row.base = obj->GetFormID();
                    row.isArmor = g_gems[it->second.first].def->isArmor;
                    row.isSupport = g_gems[it->second.first].def->isSupport;
                    row.label = obj->GetName();
                    if (plain > 1) {
                        row.label += std::format("  x{}", plain);
                    }
                    row.gemIdx = it->second.first;
                    row.level = it->second.second;
                    row.theme = static_cast<int>(g_gems[it->second.first].def->theme);
                    gems.push_back(std::move(row));
                }

    };
    auto inv = player->GetInventory([](RE::TESBoundObject& o) {
        return o.Is(RE::FormType::Weapon) || o.Is(RE::FormType::Armor) ||
               o.Is(RE::FormType::Misc) || o.Is(RE::FormType::SoulGem);  // m25: fuel list
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
                            if (rg.def->isSupport &&
                                rg.def->supportType == meo::SupportType::kConduit) {
                                row.hasConduit = true;  // m36: adapts off-domain gems
                            }
                            row.slotGem[s] = std::format("{} {}", GemName(rg),
                                                         meo::kRoman[it->second.level - 1]);
                            row.slotGemIdx[s] = gemIt->second;
                            row.slotLevel[s] = it->second.level;
                            row.slotTheme[s] = static_cast<int>(rg.def->theme);
                            row.slotXp[s] = it->second.xp;
                            row.slotNeed[s] = NextThreshold(rg.def, it->second.level);
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
        } else if (auto* sg = obj->As<RE::TESSoulGem>(); sg) {
            // m25 station redesign: the right pane's fuel list. Reusable gems
            // (Azura's Star) stay excluded, matching the feed path.
            if (g_reusableSoulGemKW && sg->HasKeyword(g_reusableSoulGemKW)) {
                continue;
            }
            const int   baseSoul = static_cast<int>(sg->GetContainedSoul());
            std::int32_t plainCnt = data.first;
            std::array<std::int32_t, 6> bySoul{};
            if (data.second && data.second->extraLists) {
                for (auto* xs : *data.second->extraLists) {
                    if (!xs) {
                        continue;
                    }
                    const std::int32_t c = std::max(xs->GetCount(), 1);
                    int s = baseSoul;
                    if (auto* es = xs->GetByType<RE::ExtraSoul>()) {
                        s = static_cast<int>(es->GetContainedSoul());
                    }
                    if (s >= 1 && s <= 5) {
                        bySoul[s] += c;
                    }
                    plainCnt -= c;
                }
            }
            if (plainCnt > 0 && baseSoul >= 1 && baseSoul <= 5) {
                bySoul[baseSoul] += plainCnt;
            }
            for (int s = 1; s <= 5; ++s) {
                if (bySoul[s] <= 0) {
                    continue;
                }
                MenuSoulRow row;
                row.base = obj->GetFormID();
                row.soul = s;
                row.label = obj->GetName();
                if (bySoul[s] > 1) {
                    row.label += std::format("  x{}", bySoul[s]);
                }
                souls.push_back(std::move(row));
            }
        } else {
            collectGemRows(obj, data.first, data.second.get());
        }
    }
    if (auto* pouch = PouchRef()) {
        for (const auto& [obj, data] : pouch->GetInventory(
                 [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
            if (data.first > 0) {
                collectGemRows(obj, data.first, data.second.get());
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
    std::sort(souls.begin(), souls.end(), [](const MenuSoulRow& a, const MenuSoulRow& b) {
        return a.soul != b.soul ? a.soul < b.soul : a.label < b.label;
    });
    g_menu.items = std::move(items);
    g_menu.souls = std::move(souls);
    g_menu.gems = std::move(gems);
    int found = -1;
    if (g_menu.selBase) {  // re-locate the selected ITEM after the resort
        for (int i = 0; i < static_cast<int>(g_menu.items.size()); ++i) {
            if (g_menu.items[i].base == g_menu.selBase && g_menu.items[i].uid == g_menu.selUid) {
                found = i;
                break;
            }
        }
    }
    g_menu.selItem = (found >= 0) ? found
                                  : std::clamp(g_menu.selItem, 0,
                                               std::max(0, static_cast<int>(g_menu.items.size()) - 1));
    if (found < 0 && g_menu.selItem < static_cast<int>(g_menu.items.size()) &&
        !g_menu.items.empty()) {  // keep identity in sync with the fallback row
        g_menu.selBase = g_menu.items[g_menu.selItem].base;
        g_menu.selUid = g_menu.items[g_menu.selItem].uid;
    }
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
        EquipCycleWorn(player, form, xl);  // m23c: real teardown — Update alone
                                           // leaves the removed gem's ability live
        if (WornGidCount(rec.gid) >= 2) {  // m35c: removal may un-cap a 3rd copy elsewhere
            ReapplyWornSockets(true, true, false);
        }
    }
    spdlog::info("[menu] unsocketed {:08X}/{}[{}]: '{}' L{} xp={:.0f}", a_base, a_uid, a_slot,
                 rec.gid, rec.level, rec.xp);
    GiveGemInstance(gemIt->second, rec.level, rec.xp);
    const auto& rg = g_gems[gemIt->second];
    Notify(std::format("{} {} returned to your pouch.", GemName(rg), meo::kRoman[rec.level - 1]));
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
        EquipCycleWorn(player, form, xl);  // m23c: same stale-ability teardown as unsocket
        if (WornGidCount(rec.gid) >= 2) {  // m35c: destroying may un-cap a 3rd copy elsewhere
            ReapplyWornSockets(true, true, false);
        }
    }
    if (tier >= 0 && g_filledSoulGems[tier]) {
        player->AddObjectToContainer(g_filledSoulGems[tier], nullptr, 1, nullptr);
        Notify(std::format("Gem destroyed — its essence yields a {} soul gem.", kSoulNames[tier]));
    } else {
        Notify("Gem destroyed — too little essence to reclaim a soul.");
    }
    GrantEnchantingXP(g_destroySkillXP * rec.level);  // m37: breaking a gem down teaches its craft (× level)
    spdlog::info("[destroy] {:08X}/{} '{}' L{} xp={:.0f} -> soul tier {}", a_base, a_uid, rec.gid,
                 rec.level, rec.xp, tier);
}

void MenuSocket(RE::FormID a_itemBase, std::uint16_t a_itemUid, RE::FormID a_gemBase,
                std::uint16_t a_gemUid, int a_targetSlot = -1) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* itemForm = RE::TESForm::LookupByID<RE::TESBoundObject>(a_itemBase);
    auto* gemForm = RE::TESForm::LookupByID<RE::TESObjectMISC>(a_gemBase);
    auto  gemMapIt = g_gemByItem.find(a_gemBase);
    if (!player || !itemForm || !gemForm || gemMapIt == g_gemByItem.end()) {
        return;
    }
    const bool gemIsSupport = g_gems[gemMapIt->second.first].def->isSupport;
    const bool gemIsArmor   = g_gems[gemMapIt->second.first].def->isArmor;
    // Support gems (m36) are domain-agnostic but only work in a dual-socket
    // (linked) item. The normal-gem domain rule is checked AFTER the uid is
    // known, because a Conduit in the item relaxes it (off-domain adapter).
    if (gemIsSupport && SocketCapacity(itemForm) < 2) {
        Notify("Support gems only work in a linked (dual-socket) item.");
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
            ref->extraList.SetOwner(player->GetActorBase());  // never theft (m17b)
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
    // Multi-socket placement. Our own combined enchant doesn't block — only a
    // foreign (player/base) one. m35e (marth: swap without removing first): when
    // a_targetSlot names a slot that already holds one of OUR gems, we evict it
    // to the pouch below and stamp the replacement in its place — the "remove +
    // apply" the player would otherwise do by hand, in one click. a_targetSlot
    // -1 keeps the old behaviour (fill the first free slot).
    auto* itemXid = xl->GetByType<RE::ExtraUniqueID>();
    const std::uint16_t uid = itemXid ? itemXid->uniqueID : 0;
    const int cap = SocketCapacity(itemForm);
    int  firstFree = -1;
    bool ourSockets = false;
    for (int s = 0; s < cap; ++s) {
        if (g_sockets.contains(MakeKey(a_itemBase, uid, static_cast<std::uint8_t>(s)))) {
            ourSockets = true;
        } else if (firstFree < 0) {
            firstFree = s;
        }
    }
    if (xl->HasType(RE::ExtraDataType::kEnchantment) && !ourSockets) {
        Notify("That item is already enchanted.");
        return;
    }
    const int freeSlot = (a_targetSlot >= 0 && a_targetSlot < cap) ? a_targetSlot : firstFree;
    if (freeSlot < 0) {
        Notify(cap > 1 ? "Every socket is already filled." : "That item's socket is filled.");
        return;
    }
    // m36: one support per item — reject a second support in any OTHER slot (the
    // placement slot itself may hold a support we're swapping out). Two supports
    // would both be inert (DESIGN §5).
    if (gemIsSupport) {
        for (int s = 0; s < cap; ++s) {
            if (s == freeSlot) {
                continue;
            }
            auto sit = g_sockets.find(MakeKey(a_itemBase, uid, static_cast<std::uint8_t>(s)));
            if (sit == g_sockets.end()) {
                continue;
            }
            auto sgi = g_gemByGid.find(sit->second.gid);
            if (sgi != g_gemByGid.end() && g_gems[sgi->second].def->isSupport) {
                Notify("An item can hold only one support gem.");
                return;
            }
        }
    }
    // m36: the normal-gem domain rule — off-domain is allowed ONLY when a Conduit
    // is present in the item to adapt it (DESIGN §5). A plain (Conduit-less) item
    // still refuses a mismatched gem.
    if (!gemIsSupport && gemIsArmor != itemForm->Is(RE::FormType::Armor) &&
        !ItemHasConduit(a_itemBase, uid)) {
        Notify("That gem doesn't fit that kind of gear.");
        return;
    }
    const auto [gemIdx, itemLevel] = gemMapIt->second;
    int          level = itemLevel;
    float        xp = 0.0f;
    bool         hadRec = false;
    SocketRecord saved{};
    RE::TESObjectREFR* gemHolder = player;  // m27: gems normally live in the pouch
    RE::ExtraDataList* gemXL = nullptr;
    std::uint16_t      useUid = a_gemUid;  // m35: the record's actual key (may drift)
    if (a_gemUid) {
        auto* pouch = PouchRef();
        if (pouch) {
            gemXL = FindInstanceXList(pouch, gemForm, a_gemUid);
            if (gemXL) {
                gemHolder = pouch;
            }
        }
        if (!gemXL) {
            gemXL = FindInstanceXList(player, gemForm, a_gemUid);
        }
        // m32h: the menu's uid can drift a frame behind an in-flight pouch
        // route. If the exact uid is gone, re-locate the SAME leveled gem —
        // any recorded instance of this base — and use its current uid.
        if (!gemXL) {
            auto relocate = [&](RE::TESObjectREFR* a_h) -> RE::ExtraDataList* {
                auto* ch = a_h ? a_h->GetInventoryChanges() : nullptr;
                if (!ch || !ch->entryList) {
                    return nullptr;
                }
                for (auto* e : *ch->entryList) {
                    if (!e || e->object != gemForm || !e->extraLists) {
                        continue;
                    }
                    for (auto* xl2 : *e->extraLists) {
                        auto* xid = xl2 ? xl2->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (xid && g_sockets.contains(MakeKey(a_gemBase, xid->uniqueID))) {
                            useUid = xid->uniqueID;
                            return xl2;
                        }
                    }
                }
                return nullptr;
            };
            gemXL = relocate(pouch);
            if (gemXL) {
                gemHolder = pouch;
            } else {
                gemXL = relocate(player);
            }
            if (gemXL && useUid != a_gemUid) {
                spdlog::info("[menu] gem uid drifted {} -> {} (pouch/inv sync)", a_gemUid, useUid);
            }
        }
        if (auto it = g_sockets.find(MakeKey(a_gemBase, useUid)); it != g_sockets.end()) {
            saved = it->second;
            hadRec = true;
            level = saved.level;
            xp = saved.xp;
            g_sockets.erase(it);
        }
        if (!gemXL) {
            spdlog::warn("[menu] socket gem {:08X}/{} not found in pouch or inventory",
                         a_gemBase, a_gemUid);
            Notify("That gem is no longer in your pouch.");
            if (hadRec) {
                g_sockets[MakeKey(a_gemBase, useUid)] = saved;
            }
            return;
        }
    } else {
        auto holds = [&](RE::TESObjectREFR* a_h) {
            return a_h && !a_h->GetInventoryCounts(
                              [&](RE::TESBoundObject& o) { return &o == gemForm; }).empty();
        };
        if (holds(PouchRef())) {
            gemHolder = PouchRef();
        } else if (!holds(player)) {
            Notify("That gem is no longer in your pouch.");
            return;
        }
    }
    // m35e swap: the target slot may already hold one of our gems (a full-item
    // swap). Return that gem to the pouch before stamping the replacement — the
    // remove half of the one-click swap. Done AFTER the incoming gem is fully
    // resolved so a failed lookup above aborts before we disturb the old gem.
    if (auto occ = g_sockets.find(MakeKey(a_itemBase, uid, static_cast<std::uint8_t>(freeSlot)));
        occ != g_sockets.end()) {
        const SocketRecord oldRec = occ->second;
        auto               oldIt = g_gemByGid.find(oldRec.gid);
        g_sockets.erase(occ);
        if (oldIt != g_gemByGid.end()) {
            GiveGemInstance(oldIt->second, oldRec.level, oldRec.xp);
            const auto& oldRg = g_gems[oldIt->second];
            Notify(std::format("{} {} returned to your pouch.", GemName(oldRg),
                               meo::kRoman[std::clamp<int>(oldRec.level, 1, 5) - 1]));
        }
        spdlog::info("[menu] swap: evicted '{}' L{} from {:08X}/{}[{}]", oldRec.gid,
                     oldRec.level, a_itemBase, uid, freeSlot);
    }
    if (!StampInstance(itemForm, xl, gemIdx, level, static_cast<std::uint8_t>(freeSlot), xp)) {
        if (hadRec) {
            g_sockets[MakeKey(a_gemBase, useUid)] = saved;  // m35: restore under the drift-corrected key
        }
        return;
    }
    if (IsWornXList(xl)) {
        if (WornGidCount(g_gems[gemIdx].def->gid) > 2) {
            ReapplyWornSockets(true, true, false);  // m35c: a 3rd copy — redistribute the cap
        } else {
            ApplyWornAbility(player, itemForm, xl, xl->HasType(RE::ExtraDataType::kWornLeft));
        }
    }
    gemHolder->RemoveItem(gemForm, 1, RE::ITEM_REMOVE_REASON::kRemove, gemXL, nullptr);
    const auto& rg = g_gems[gemIdx];
    Notify(std::format("{} {} socketed into {}.", GemName(rg), meo::kRoman[level - 1],
                       itemForm->GetName()));
}

// M10 (stage 2a): consume the smallest filled, non-reusable soul gem and
// grant its Gem XP (DESIGN §3) to one socketed gem. Enchanting-station only.
// a_soulLevel 0 = auto (smallest filled wins); 1..5 = feed exactly that tier
// (m25 station redesign: the player clicks the soul gem to burn).
void FeedSoulToGem(RE::FormID a_itemBase, std::uint16_t a_uid, std::uint8_t a_slot,
                   int a_soulLevel = 0) {
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
        if (a_soulLevel > 0 && soul != a_soulLevel) {
            return;  // m25: the menu names the exact tier to burn
        }
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
    // m25 (marth: "this needs to yield more"): soul feeding IS this list's
    // enchanting practice — MEO replaced the vanilla table flow that used to
    // train the skill, so the skill trains here. Roughly a vanilla enchant's
    // worth per soul of the same size, MCM-scalable.
    static constexpr float kSoulSkillXP[5] = { 15.0f, 35.0f, 75.0f, 125.0f, 200.0f };
    if (g_enchSkillXPMult > 0.0f) {
        player->AddSkillExperience(RE::ActorValue::kEnchanting,
                                   kSoulSkillXP[idx] * g_enchSkillXPMult);
    }
    auto gemIt = g_gemByGid.find(recIt->second.gid);
    if (gemIt == g_gemByGid.end()) {
        return;
    }
    const bool left = xl->HasType(RE::ExtraDataType::kWornLeft);
    const bool grew = GrantGemXP(player, itemForm, xl, left, recIt->second, gemIt->second, xp, a_uid);
    Notify(grew ? std::format("Fed a {} soul (+{:.0f} Gem XP).", kSoulNames[idx], xp)
                : "That gem is already mastered — the soul is spent for nothing.");
    spdlog::info("[feed] {} soul -> {:08X}/{} : +{:.0f} gem xp, +{:.0f} ench skill xp",
                 kSoulNames[idx], a_itemBase, a_uid, xp,
                 kSoulSkillXP[idx] * g_enchSkillXPMult);
}

void QueueMenuTask(std::function<void()> a_fn) {
    g_menu.busy = true;
    SKSE::GetTaskInterface()->AddTask([fn = std::move(a_fn)]() {
        fn();
        // m24c (marth: adding a 2nd gem "changed" the 1st gem's magnitude):
        // Update*Ability's replace can leave the PREVIOUS combined ability
        // alive next to the new one — two Resist Fire entries summing in the
        // effects list. Same engine gap as the unsocket orphan, socket
        // direction. Sweep after every menu action, not just on load.
        DispelStaleGemEffects();
        BuildMenuSnapshot();
        g_menu.busy = false;
    });
}

void CloseGemMenu() {
    const bool wasStation = g_menu.station.load();
    g_menu.open = false;
    g_menu.wantClose = false;
    // m18b: in takeover mode the vanilla CraftingMenu was hidden at open, so
    // its normal exit never runs and the player stays locked at the bench.
    // Force the furniture exit ourselves (the standard forced-idle release).
    if (wasStation && g_stationTakeover) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && player->GetOccupiedFurniture()) {
                player->NotifyAnimationGraph("IdleForceDefaultState");
            }
        });
    }
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
    // m23c: real typefaces, loaded at init scaled to the backbuffer (the
    // upscaled default bitmap font was the menu's biggest visual weakness).
    // Optional files — absent means ImGui default at the legacy global scale.
    ImFont* g_fontBody = nullptr;
    ImFont* g_fontHead = nullptr;
    ImFont* g_fontSans = nullptr;  // Quicksilver skin face (m24)
    // m23c input-quality state (see InputDispatchHook / DrawGemMenu):
    // shout-key close triggers on RELEASE with both edges swallowed, gated on
    // a press seen while open; the cursor position is pushed to ImGui on every
    // open so the first click lands even before the mouse ever moves.
    std::atomic<bool>          g_shoutDownSeen{ false };
    std::atomic<bool>          g_cursorInit{ false };
    std::atomic<float>         g_cursorX{ -1.0f };  // m35: read on render thread, written on input thread
    std::atomic<float>         g_cursorY{ -1.0f };
    std::atomic<std::uint64_t> g_destroyArm{ 0 };  // armed Destroy row (two-click confirm)
    // m32f controller: left-stick nav state (edge-triggered -> dpad keys;
    // ImGui's own nav repeat handles held directions)
    bool g_stickNav[4] = { false, false, false, false };  // up, down, left, right

    // Accent per GemCatalog Theme (order frozen: kFire..kUtility). The one
    // visual anchor every style direction shares — gems read by color.
    inline constexpr ImVec4 kThemeCol[9] = {
        { 0.90f, 0.42f, 0.18f, 1.0f },  // Fire     — ember
        { 0.45f, 0.72f, 0.88f, 1.0f },  // Frost    — glacial
        { 0.62f, 0.62f, 0.95f, 1.0f },  // Shock    — arc indigo
        { 0.68f, 0.50f, 0.88f, 1.0f },  // Arcane   — violet
        { 0.80f, 0.28f, 0.38f, 1.0f },  // Drain    — crimson
        { 0.78f, 0.72f, 0.58f, 1.0f },  // Martial  — steel-tan
        { 0.52f, 0.70f, 0.42f, 1.0f },  // Roguish  — moss
        { 0.92f, 0.83f, 0.55f, 1.0f },  // Holy     — pale gold
        { 0.42f, 0.72f, 0.65f, 1.0f },  // Utility  — teal
    };
    ImVec4 ThemeCol(int a_theme) {
        return (a_theme >= 0 && a_theme < 9) ? kThemeCol[a_theme]
                                             : ImVec4(0.70f, 0.68f, 0.62f, 1.0f);
    }

    // Socket pip / gem swatch: a small diamond, the mod's own glyph — drawn
    // with primitives so no bundled font has to cover U+25C6.
    void DrawDiamond(ImDrawList* a_dl, ImVec2 a_c, float a_r, ImU32 a_col, bool a_filled) {
        const ImVec2 p0(a_c.x, a_c.y - a_r), p1(a_c.x + a_r, a_c.y),
            p2(a_c.x, a_c.y + a_r), p3(a_c.x - a_r, a_c.y);
        if (a_filled) {
            a_dl->AddQuadFilled(p0, p1, p2, p3, a_col);
        } else {
            a_dl->AddQuad(p0, p1, p2, p3, a_col, 1.4f);
        }
    }

    // m24: four runtime skins, an MCM dropdown away (marth's "diabolical
    // idea" — ship ALL directions, pick live). Palettes are the mockup
    // artifact's values verbatim; the nine gem THEME colors stay fixed
    // across skins so gems always read by color. Square corners and flat
    // fills throughout — ImGui's honest range, closer to Skyrim's UI
    // language than its default debug grey.
    struct MenuSkin {
        const char* name;
        ImVec4      winBg, panel, border, text, dim, sel, accent, btn, track, danger;
        bool        sans;   // Quicksilver: sans face + spaced HUD title
        const char* title;
    };
    inline constexpr MenuSkin kSkins[4] = {
        { "Ebony & Brass",
          { 0.04f, 0.04f, 0.06f, 0.95f }, { 0.07f, 0.07f, 0.10f, 0.95f },
          { 0.55f, 0.48f, 0.27f, 0.60f }, { 0.91f, 0.89f, 0.84f, 1.00f },
          { 0.58f, 0.55f, 0.47f, 1.00f }, { 0.34f, 0.29f, 0.16f, 0.85f },
          { 0.78f, 0.70f, 0.45f, 1.00f }, { 0.13f, 0.11f, 0.07f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.08f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          false, "GEM SOCKETING" },
        { "Dwemer Parchment",
          { 0.92f, 0.88f, 0.80f, 0.97f }, { 0.95f, 0.92f, 0.85f, 1.00f },
          { 0.54f, 0.45f, 0.25f, 0.85f }, { 0.21f, 0.17f, 0.12f, 1.00f },
          { 0.48f, 0.43f, 0.34f, 1.00f }, { 0.86f, 0.81f, 0.66f, 1.00f },
          { 0.43f, 0.29f, 0.16f, 1.00f }, { 0.89f, 0.84f, 0.72f, 1.00f },
          { 0.00f, 0.00f, 0.00f, 0.10f }, { 0.55f, 0.23f, 0.18f, 1.00f },
          false, "GEM SOCKETING" },
        { "Soul Cairn",
          { 0.07f, 0.06f, 0.13f, 0.95f }, { 0.10f, 0.08f, 0.19f, 0.95f },
          { 0.35f, 0.31f, 0.55f, 0.70f }, { 0.85f, 0.84f, 0.92f, 1.00f },
          { 0.55f, 0.52f, 0.66f, 1.00f }, { 0.16f, 0.14f, 0.31f, 0.90f },
          { 0.53f, 0.85f, 0.92f, 1.00f }, { 0.13f, 0.10f, 0.23f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.08f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          false, "GEM SOCKETING" },
        { "Quicksilver",
          { 0.04f, 0.05f, 0.06f, 0.88f }, { 0.07f, 0.08f, 0.10f, 0.92f },
          { 0.22f, 0.25f, 0.29f, 1.00f }, { 0.83f, 0.85f, 0.88f, 1.00f },
          { 0.47f, 0.50f, 0.54f, 1.00f }, { 0.14f, 0.19f, 0.23f, 0.90f },
          { 0.56f, 0.72f, 0.80f, 1.00f }, { 0.09f, 0.11f, 0.13f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.07f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          true, "G E M   S O C K E T I N G" },
    };
    ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w };
    }
    int g_appliedSkin = -1;

    void ApplyMenuStyle(const MenuSkin& s) {
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.WindowPadding = ImVec2(18.0f, 14.0f);
        style.ItemSpacing = ImVec2(10.0f, 7.0f);
        style.FramePadding = ImVec2(10.0f, 7.0f);
        style.ScrollbarSize = 14.0f;
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
        auto* c = style.Colors;
        c[ImGuiCol_WindowBg]         = s.winBg;
        c[ImGuiCol_ChildBg]          = s.panel;
        c[ImGuiCol_PopupBg]          = s.panel;  // tooltips follow the skin
        c[ImGuiCol_Border]           = s.border;
        c[ImGuiCol_Separator]        = s.border;
        c[ImGuiCol_Text]             = s.text;
        c[ImGuiCol_TextDisabled]     = s.dim;
        c[ImGuiCol_Header]           = s.sel;
        c[ImGuiCol_HeaderHovered]    = Mix(s.sel, s.accent, 0.25f);
        c[ImGuiCol_HeaderActive]     = Mix(s.sel, s.accent, 0.40f);
        c[ImGuiCol_Button]           = s.btn;
        c[ImGuiCol_ButtonHovered]    = Mix(s.btn, s.accent, 0.25f);
        c[ImGuiCol_ButtonActive]     = Mix(s.btn, s.accent, 0.40f);
        c[ImGuiCol_ScrollbarBg]      = s.track;
        c[ImGuiCol_ScrollbarGrab]    = ImVec4(s.dim.x, s.dim.y, s.dim.z, 0.60f);
        c[ImGuiCol_TitleBg]          = s.winBg;
        c[ImGuiCol_TitleBgActive]    = s.winBg;
        c[ImGuiCol_NavHighlight]     = s.accent;  // m32f: controller focus ring
    }

    void DrawGemMenu() {
        auto& io = ImGui::GetIO();
        if (g_menu.wantClose.exchange(false)) {
            CloseGemMenu();
            return;
        }
        io.MouseDrawCursor = true;
        // m24: skin from the MCM dropdown; restyle only when it changes.
        const int       skinIdx = std::clamp(g_menuStyle, 0, 3);
        const MenuSkin& skin = kSkins[skinIdx];
        if (g_appliedSkin != skinIdx) {
            ApplyMenuStyle(skin);
            g_appliedSkin = skinIdx;
            spdlog::info("[menu] skin: {}", skin.name);
        }
        ImFont* fBody = skin.sans ? (g_fontSans ? g_fontSans : g_fontBody) : g_fontBody;
        ImFont* fHead = skin.sans ? fBody : (g_fontHead ? g_fontHead : g_fontBody);
        // With a real typeface the font is baked at backbuffer scale; the
        // global scale is only the legacy fallback for a missing font file.
        io.FontGlobalScale = fBody ? 1.0f : std::max(1.0f, io.DisplaySize.y / 1080.0f);
        if (fBody) {
            ImGui::PushFont(fBody);
        }
        // Centered on each open (Appearing, not Always) so it can be
        // dragged afterwards; DisplaySize is backbuffer-true by now.
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.68f),
                                 ImGuiCond_Appearing);
        // m24c: resizable (drag any edge); ImGui keeps the chosen size for
        // the rest of the session since the window persists in the context.
        ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 420.0f),
                                            ImVec2(io.DisplaySize.x, io.DisplaySize.y));
        if (!ImGui::Begin("Gem Socketing", nullptr,
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                              ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            if (fBody) {
                ImGui::PopFont();
            }
            return;
        }
        auto* dl = ImGui::GetWindowDrawList();
        const float lineH = ImGui::GetTextLineHeight();
        // m29 (marth: "gains X at gem level IV" must be readable in game):
        // hovering a gem or a filled socket lists the rank ladder's grants,
        // pulled from each rung MGEF's own description — per-list truth.
        auto gemBasics = [&](int a_idx, int a_level) -> std::string {
            const auto& rg = g_gems[a_idx];
            const int   li = std::clamp(a_level, 1, 5) - 1;
            auto*       m = rg.mgefLv[li] ? rg.mgefLv[li] : rg.mgef;
            const float mag = GemBaseMag(rg.def, li) * g_magnitudeMult *
                              (1.0f + 0.08f * g_attuneRank) * GemPerkMult(rg.def);
            const char* d = m ? m->magicItemDescription.c_str() : nullptr;
            if (d && *d) {
                std::string s(d);
                if (const auto dot = s.find(". "); dot != std::string::npos) {
                    s = s.substr(0, dot + 1);  // the base line only
                }
                const std::string tok = "<mag>";
                if (const auto p = s.find(tok); p != std::string::npos) {
                    s = s.substr(0, p) + std::format("{:.0f}", mag) + s.substr(p + tok.size());
                }
                std::erase(s, '<');
                std::erase(s, '>');
                return s;
            }
            return std::format("{}: {:.0f}", GemName(rg), mag);
        };
        auto rungTooltip = [&](int a_gemIdx, int a_level, const std::string& a_action) {
            if (!ImGui::IsItemHovered()) {
                return;
            }
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(a_action.c_str());
            if (a_gemIdx >= 0 && a_gemIdx < static_cast<int>(g_gems.size())) {
                ImGui::TextUnformatted(gemBasics(a_gemIdx, a_level).c_str());  // m32c
            }
            if (a_gemIdx >= 0 && a_gemIdx < static_cast<int>(g_gems.size())) {
                for (const auto& [lv, note] : g_gems[a_gemIdx].lvNotes) {
                    const bool active = a_level >= lv;
                    ImGui::TextColored(active ? skin.accent : ImGui::GetStyleColorVec4(
                                                                  ImGuiCol_TextDisabled),
                                       "Level %s: %s%s", meo::kRoman[lv - 1], note.c_str(),
                                       active ? "  (active)" : "");
                }
            }
            ImGui::EndTooltip();
        };
        {  // Title in the skin's display face, flanked by drawn rules.
            const char* title = skin.title;
            if (fHead) {
                ImGui::PushFont(fHead);
            }
            const ImVec2 ts = ImGui::CalcTextSize(title);
            const float  tx = (ImGui::GetWindowSize().x - ts.x) * 0.5f;
            const ImVec2 wp = ImGui::GetWindowPos();
            const float  ry = wp.y + ImGui::GetCursorPosY() + ts.y * 0.58f;
            const ImU32  rule = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(wp.x + 26.0f, ry), ImVec2(wp.x + tx - 18.0f, ry), rule);
            dl->AddLine(ImVec2(wp.x + tx + ts.x + 18.0f, ry),
                        ImVec2(wp.x + ImGui::GetWindowSize().x - 26.0f, ry), rule);
            ImGui::SetCursorPosX(tx);
            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            if (fHead) {
                ImGui::PopFont();
            }
            ImGui::Spacing();
        }
        std::scoped_lock lk(g_menu.lock);
        const bool  busy = g_menu.busy.load();
        const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
        const float half = ImGui::GetContentRegionAvail().x * 0.5f;
        const float rowH = lineH + 10.0f;
        // m36e (marth: d-pad left/right only crossed panes when aligned with a
        // slotted gem — too geometry-dependent). Make left/right DETERMINISTICALLY
        // jump between the two panes: track which pane held nav focus last frame,
        // and on the opposite d-pad press request focus into the other pane.
        static int s_navPane = 0;  // 0 = items, 1 = gems
        int        wantPane = -1;
        if (!busy) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) && s_navPane == 0) {
                wantPane = 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) && s_navPane == 1) {
                wantPane = 0;
            }
        }
        bool itemsFocused = false;
        // Left pane: NEVER disabled — selection is pure UI state (identity-
        // tracked across rebuilds since m19e), and eating clicks during the
        // brief busy window read as "the menu misses clicks" in the field.
        ImGui::BeginChild("items", ImVec2(half - 6.0f, -footer),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);  // m32f
        // m24c (marth: long lists "leave the pane"): rows were drawn through
        // the OUTER window's draw list, which ignores the child's clip rect.
        // Each pane draws through its own list so scrolled-out rows clip.
        auto* dlL = ImGui::GetWindowDrawList();
        ImGui::TextDisabled("SOCKETABLE ITEMS");
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(g_menu.items.size()); ++i) {
            const auto&  row = g_menu.items[i];
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            // Selection acts on mouse PRESS (IsItemClicked), not release:
            // raw-delta cursor motion between press and release could leave
            // the row and cancel a release-click — the missed-click report.
            // The Selectable return still serves keyboard/gamepad activation.
            if (wantPane == 0 && i == g_menu.selItem) {
                ImGui::SetKeyboardFocusHere();  // m36e: land on the selected item
            }
            const bool nav = ImGui::Selectable(std::format("##item{}", i).c_str(),
                                               g_menu.selItem == i, 0, ImVec2(0.0f, rowH));
            if (ImGui::IsItemFocused()) {
                itemsFocused = true;  // m36e: this pane holds the nav cursor
            }
            if (nav || ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                g_menu.selItem = i;
                g_menu.selBase = row.base;
                g_menu.selUid = row.uid;
                g_menu.selSlot = -1;  // m25: feed target follows the item
                g_destroyArm = 0;
            }
            // Socket pips: one diamond per slot, filled+tinted when occupied.
            float cx = rp.x + 12.0f;
            for (int s = 0; s < row.capacity && s < kMaxSockets; ++s) {
                const bool has = !row.slotGem[s].empty();
                const ImU32 col = ImGui::GetColorU32(
                    has ? ThemeCol(row.slotTheme[s])
                        : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                DrawDiamond(dlL, ImVec2(cx, rp.y + rowH * 0.5f), 4.5f, col, has);
                cx += 13.0f;
            }
            dlL->AddText(ImVec2(rp.x + 42.0f, rp.y + (rowH - lineH) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
        }
        if (g_menu.items.empty()) {
            ImGui::TextDisabled("No socketable items.");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("gems", ImVec2(0, -footer),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        auto* dlR = ImGui::GetWindowDrawList();  // m24c: pane-clipped drawing
        const float innerW = ImGui::GetContentRegionAvail().x;
        if (wantPane == 1) {
            ImGui::SetKeyboardFocusHere();  // m36e: jump to the first row of this pane
        }
        if (busy) {
            ImGui::BeginDisabled();
        }
        if (g_menu.selItem >= 0 && g_menu.selItem < static_cast<int>(g_menu.items.size())) {
            const auto sel = g_menu.items[g_menu.selItem];  // copy: queue may rebuild
            ImGui::TextDisabled("%s%s", sel.label.c_str(),
                                sel.capacity > 1 ? "  — 2 linked sockets" : "");
            ImGui::Separator();
            // m25 station redesign (marth): at a bench the right pane is
            // SELECT-a-gem (top, highlight like the item pane) + the SOUL GEM
            // list (below) — click a soul to burn it into the selected gem.
            // Pouch mode: click a filled socket to remove; pick a loose gem to
            // socket an empty slot OR swap into a full one (m35e).
            const bool station = g_menu.station.load();
            for (int s = 0; s < sel.capacity && s < kMaxSockets; ++s) {
                ImGui::PushID(s);
                const ImVec2 rp = ImGui::GetCursorScreenPos();
                if (sel.slotGem[s].empty()) {
                    ImGui::Dummy(ImVec2(0.0f, rowH));
                    DrawDiamond(dlR, ImVec2(rp.x + 12.0f, rp.y + rowH * 0.5f), 4.5f,
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), false);
                    dlR->AddText(ImVec2(rp.x + 28.0f, rp.y + (rowH - lineH) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                std::format("Socket {} — empty", s + 1).c_str());
                    ImGui::PopID();
                    continue;
                }
                const ImVec4 tcol = ThemeCol(sel.slotTheme[s]);
                const bool   picked = station && g_menu.selSlot == s;
                const bool   nav = ImGui::Selectable("##slot", picked, 0, ImVec2(0.0f, rowH));
                const bool   act = nav || ImGui::IsItemClicked(ImGuiMouseButton_Left);
                rungTooltip(sel.slotGemIdx[s], sel.slotLevel[s],
                            std::string(station ? "Select " : "Remove ") + sel.slotGem[s]);
                DrawDiamond(dlR, ImVec2(rp.x + 12.0f, rp.y + rowH * 0.5f), 5.0f,
                            ImGui::GetColorU32(tcol), true);
                dlR->AddText(ImVec2(rp.x + 28.0f, rp.y + (rowH - lineH) * 0.5f),
                            ImGui::GetColorU32(tcol), sel.slotGem[s].c_str());
                if (sel.slotNeed[s] > 0.0f) {
                    const std::string xps =
                        std::format("{:.0f} / {:.0f}", sel.slotXp[s], sel.slotNeed[s]);
                    dlR->AddText(ImVec2(rp.x + innerW - ImGui::CalcTextSize(xps.c_str()).x - 8.0f,
                                       rp.y + (rowH - lineH) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), xps.c_str());
                    const float bx0 = rp.x + 28.0f;
                    const float bx1 = rp.x + innerW - 8.0f;
                    const float by = rp.y + rowH - 3.0f;
                    dlR->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 2.0f),
                                      ImGui::GetColorU32(ImGuiCol_ScrollbarBg));
                    dlR->AddRectFilled(
                        ImVec2(bx0, by),
                        ImVec2(bx0 + (bx1 - bx0) *
                                         std::clamp(sel.slotXp[s] / sel.slotNeed[s], 0.0f, 1.0f),
                               by + 2.0f),
                        ImGui::GetColorU32(tcol));
                } else {
                    dlR->AddText(ImVec2(rp.x + innerW - ImGui::CalcTextSize("mastered").x - 8.0f,
                                       rp.y + (rowH - lineH) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "mastered");
                }
                if (act) {
                    if (station) {
                        g_menu.selSlot = s;
                        g_destroyArm = 0;
                    } else {
                        const std::uint8_t slot = static_cast<std::uint8_t>(s);
                        g_destroyArm = 0;
                        QueueMenuTask([sel, slot]() { MenuUnsocket(sel.base, sel.uid, slot); });
                    }
                }
                ImGui::PopID();
            }
            if (station) {
                const bool haveTarget = g_menu.selSlot >= 0 && g_menu.selSlot < sel.capacity &&
                                        !sel.slotGem[g_menu.selSlot].empty();
                if (haveTarget) {
                    const std::uint8_t slot = static_cast<std::uint8_t>(g_menu.selSlot);
                    ImGui::Indent(28.0f);
                    if (ImGui::SmallButton("Unsocket")) {
                        g_destroyArm = 0;
                        g_menu.selSlot = -1;
                        QueueMenuTask([sel, slot]() { MenuUnsocket(sel.base, sel.uid, slot); });
                    }
                    ImGui::SameLine();
                    // Destroy stays the menu's one irreversible act — 2-click.
                    const InstKey key = MakeKey(sel.base, sel.uid, slot);
                    const bool    armed = g_destroyArm.load() == key;
                    if (armed) {
                        ImGui::PushStyleColor(ImGuiCol_Text, skin.danger);
                    }
                    if (ImGui::SmallButton(armed ? "Confirm destroy" : "Destroy")) {
                        if (armed) {
                            g_destroyArm = 0;
                            g_menu.selSlot = -1;
                            QueueMenuTask([sel, slot]() { DestroyGem(sel.base, sel.uid, slot); });
                        } else {
                            g_destroyArm = key;
                        }
                    }
                    if (armed) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::Unindent(28.0f);
                }
                ImGui::Separator();
                ImGui::TextDisabled("SOUL GEMS");
                if (!haveTarget) {
                    ImGui::TextDisabled("Select a socketed gem above to feed.");
                }
                int shownSouls = 0;
                for (int i = 0; i < static_cast<int>(g_menu.souls.size()); ++i) {
                    const auto soul = g_menu.souls[i];  // copy for the closure
                    ++shownSouls;
                    const ImVec2 rp = ImGui::GetCursorScreenPos();
                    ImGui::PushID(2000 + i);
                    const bool nav = ImGui::Selectable("##soul", false, 0, ImVec2(0.0f, rowH));
                    const bool act = nav || ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    ImGui::PopID();
                    // Bigger soul = bigger diamond; dimmed until a target gem
                    // is selected above.
                    const ImVec4 sc = skin.accent;
                    DrawDiamond(dlR, ImVec2(rp.x + 12.0f, rp.y + rowH * 0.5f),
                                3.0f + 0.7f * static_cast<float>(soul.soul),
                                ImGui::GetColorU32(ImVec4(sc.x, sc.y, sc.z,
                                                          haveTarget ? 1.0f : 0.45f)),
                                true);
                    dlR->AddText(ImVec2(rp.x + 28.0f, rp.y + (rowH - lineH) * 0.5f),
                                ImGui::GetColorU32(haveTarget ? ImGuiCol_Text
                                                              : ImGuiCol_TextDisabled),
                                soul.label.c_str());
                    const std::string gain = std::format(
                        "+{:.0f} gem xp", kSoulFeedXP[soul.soul - 1] *
                                              (g_hasSoulFeeder ? 2.0f : 1.0f));
                    dlR->AddText(ImVec2(rp.x + innerW - ImGui::CalcTextSize(gain.c_str()).x - 8.0f,
                                       rp.y + (rowH - lineH) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), gain.c_str());
                    if (act && haveTarget) {
                        const std::uint8_t slot = static_cast<std::uint8_t>(g_menu.selSlot);
                        const int          tier = soul.soul;
                        QueueMenuTask([sel, slot, tier]() {
                            FeedSoulToGem(sel.base, sel.uid, slot, tier);
                        });
                    }
                }
                if (shownSouls == 0) {
                    ImGui::TextDisabled("No filled soul gems in your inventory.");
                }
            } else {
            ImGui::Separator();
            // m35e (marth): the loose-gem list is ALWAYS shown, even with every
            // socket filled — picking a gem SWAPS it in (remove + apply in one
            // click, the old gem returns to the pouch). Target slot = first
            // empty one; if all are full it's a swap into socket 1 (capacity 1
            // is unambiguous; on a 2-socket item, remove the socket you want
            // gone first to target it precisely).
            int  target = -1;
            for (int s = 0; s < sel.capacity && s < kMaxSockets; ++s) {
                if (sel.slotGem[s].empty()) { target = s; break; }
            }
            const bool swapping = target < 0;
            if (swapping) {
                target = 0;  // all filled → replace socket 1 by default
                if (sel.capacity > 1) {
                    ImGui::TextDisabled("SWAP — replaces socket 1 (remove a socket above to target it)");
                } else {
                    ImGui::TextDisabled("SWAP — pick a gem to replace the current one");
                }
            } else {
                ImGui::TextDisabled(sel.isArmor ? "ARMOR GEMS" : "WEAPON GEMS");
            }
            int shown = 0;
            for (int i = 0; i < static_cast<int>(g_menu.gems.size()); ++i) {
                const auto gem = g_menu.gems[i];  // copy for the closure
                // m36: support gems are ALWAYS listed (they're valid for some
                // item; you should always see them in the pouch) — MenuSocket
                // enforces dual-only. Normal gems stay domain-locked unless a
                // Conduit in the item adapts them.
                if (!gem.isSupport && gem.isArmor != sel.isArmor && !sel.hasConduit) {
                    continue;
                }
                ++shown;
                const ImVec2 rp = ImGui::GetCursorScreenPos();
                ImGui::PushID(1000 + i);
                const bool nav = ImGui::Selectable("##gem", false, 0, ImVec2(0.0f, rowH));
                const bool act = nav || ImGui::IsItemClicked(ImGuiMouseButton_Left);
                rungTooltip(gem.gemIdx, gem.level,
                            std::format("{} {}", swapping ? "Swap in" : "Socket", gem.label));
                ImGui::PopID();
                const ImVec4 tcol = ThemeCol(gem.theme);
                // Plain gems get a slightly dimmed swatch; instances with
                // banked XP glow full and show their progress numbers.
                DrawDiamond(dlR, ImVec2(rp.x + 12.0f, rp.y + rowH * 0.5f), 4.5f,
                            ImGui::GetColorU32(ImVec4(tcol.x, tcol.y, tcol.z,
                                                      gem.uid ? 1.0f : 0.72f)),
                            true);
                dlR->AddText(ImVec2(rp.x + 28.0f, rp.y + (rowH - lineH) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), gem.label.c_str());
                if (gem.xp >= 0.0f && gem.need > 0.0f) {
                    const std::string xps = std::format("{:.0f} / {:.0f}", gem.xp, gem.need);
                    dlR->AddText(
                        ImVec2(rp.x + innerW - ImGui::CalcTextSize(xps.c_str()).x - 8.0f,
                               rp.y + (rowH - lineH) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), xps.c_str());
                }
                if (act) {
                    g_destroyArm = 0;
                    QueueMenuTask([sel, gem, target]() {
                        MenuSocket(sel.base, sel.uid, gem.base, gem.uid, target);
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
        if (busy) {
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        // m36e: remember which pane holds the nav cursor for next frame's jump.
        // A jump this frame settles next frame, so honour the request immediately.
        s_navPane = (wantPane >= 0) ? wantPane : (itemsFocused ? 0 : 1);
        if (busy) {
            ImGui::TextDisabled("Working...");
        } else if (g_menu.station.load()) {
            ImGui::TextDisabled("Click an item, then a gem. Filled sockets: click to remove; feed souls or destroy here.");
        } else {
            ImGui::TextDisabled("Click an item, then a gem to socket it. Pad: stick/d-pad move, "
                                "A select, B close. Esc or the pouch key closes.");
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
        if (ImGui::Button("Close") && !busy) {
            CloseGemMenu();
        }
        ImGui::End();
        if (fBody) {
            ImGui::PopFont();
        }
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
            // m23c: bake real typefaces at backbuffer scale (FontGlobalScale
            // on the default bitmap font was blurry at any resolution above
            // 1080p). Both files optional; the draw falls back per-font.
            {
                const float uiScale =
                    std::max(1.0f, static_cast<float>(sd.BufferDesc.Height) / 1080.0f);
                constexpr const char* kBodyTTF = "Data/SKSE/Plugins/MEO/fonts/body.ttf";
                constexpr const char* kHeadTTF = "Data/SKSE/Plugins/MEO/fonts/head.ttf";
                if (std::ifstream(kBodyTTF).good()) {
                    g_fontBody =
                        io.Fonts->AddFontFromFileTTF(kBodyTTF, std::floor(19.0f * uiScale));
                }
                if (std::ifstream(kHeadTTF).good()) {
                    g_fontHead =
                        io.Fonts->AddFontFromFileTTF(kHeadTTF, std::floor(27.0f * uiScale));
                }
                constexpr const char* kSansTTF = "Data/SKSE/Plugins/MEO/fonts/sans.ttf";
                if (std::ifstream(kSansTTF).good()) {
                    g_fontSans =
                        io.Fonts->AddFontFromFileTTF(kSansTTF, std::floor(16.5f * uiScale));
                }
                if (!g_fontHead) {
                    g_fontHead = g_fontBody;  // head falls back to body, not to default
                }
                spdlog::info("[menu] fonts: body={} head={} sans={} (scale {:.2f})",
                             g_fontBody ? "ok" : "default", g_fontHead ? "ok" : "default",
                             g_fontSans ? "ok" : "default", uiScale);
            }
            if (!ImGui_ImplWin32_Init(sd.OutputWindow) || !ImGui_ImplDX11_Init(g_device, g_context)) {
                spdlog::error("[menu] ImGui backend init failed — menu disabled");
                return;
            }
            WndProcHook::func = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                sd.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
            g_appliedSkin = std::clamp(g_menuStyle, 0, 3);
            ApplyMenuStyle(kSkins[g_appliedSkin]);
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
            // m23c: push the cursor position on open — ImGui only ever
            // learned it from move events, so the first click of a session
            // (or any click before the mouse moved) landed at an invalid
            // position and silently missed.
            if (g_cursorInit.exchange(false)) {
                if (g_cursorX < 0.0f && g_bbW > 0.0f) {
                    g_cursorX = g_bbW * 0.5f;
                    g_cursorY = g_bbH * 0.5f;
                }
                ImGui::GetIO().AddMousePosEvent(g_cursorX, g_cursorY);
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
        case K::kLeftShoulder:  return ImGuiKey_GamepadL1;
        case K::kRightShoulder: return ImGuiKey_GamepadR1;
        default:        return ImGuiKey_None;
        }
    }

    // The key currently bound to Shout/Power for a device — casting the pouch
    // power opened the menu, so the same key closes it (m18b toggle UX).
    std::uint32_t ShoutKey(RE::INPUT_DEVICE a_device) {
        auto* cm = RE::ControlMap::GetSingleton();
        auto* ue = RE::UserEvents::GetSingleton();
        if (!cm || !ue) {
            return 0xFFFFFFFFu;
        }
        const auto key = cm->GetMappedKey(ue->shout, a_device);
        return key == 0xFF ? 0xFFFFFFFFu : key;  // kInvalid guard
    }

    // Input dispatch thunk: while the menu is open, feed ImGui and swallow
    // everything so the game world stays deaf to it.
    struct InputDispatchHook {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events) {
            if (!a_events || !g_menu.open.load() || !g_d3dReady.load()) {
                func(a_source, a_events);
                return;
            }
            auto& io = ImGui::GetIO();
            if (g_cursorX < 0.0f) {
                g_cursorX = io.DisplaySize.x * 0.5f;
                g_cursorY = io.DisplaySize.y * 0.5f;
            }
            for (auto* e = *a_events; e; e = e->next) {
                if (e->eventType == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    auto* m = static_cast<RE::MouseMoveEvent*>(e);
                    g_cursorX = std::clamp(g_cursorX + static_cast<float>(m->mouseInputX), 0.0f,
                                           io.DisplaySize.x);
                    g_cursorY = std::clamp(g_cursorY + static_cast<float>(m->mouseInputY), 0.0f,
                                           io.DisplaySize.y);
                    io.AddMousePosEvent(g_cursorX, g_cursorY);
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
                        } else if (code == ShoutKey(RE::INPUT_DEVICE::kKeyboard)) {
                            // m23c: close on RELEASE, both edges swallowed.
                            // Closing on the press leaked the release to the
                            // game once the menu shut, and that release cast
                            // the pouch power again — the close-then-instant-
                            // reopen marth hit. Requiring a press seen while
                            // OPEN also keeps the release of the press that
                            // opened the menu from closing it on arrival.
                            if (down) {
                                g_shoutDownSeen = true;
                            } else if (g_shoutDownSeen.exchange(false)) {
                                g_menu.wantClose = true;
                            }
                        } else if (auto key = DIKToImGuiKey(code); key != ImGuiKey_None) {
                            io.AddKeyEvent(key, down);
                        }
                        break;
                    case RE::INPUT_DEVICE::kGamepad:
                        if (static_cast<RE::BSWin32GamepadDevice::Key>(code) ==
                                RE::BSWin32GamepadDevice::Key::kB &&
                            down) {
                            g_menu.wantClose = true;
                        } else if (code == ShoutKey(RE::INPUT_DEVICE::kGamepad)) {
                            if (down) {  // same release-toggle as keyboard
                                g_shoutDownSeen = true;
                            } else if (g_shoutDownSeen.exchange(false)) {
                                g_menu.wantClose = true;
                            }
                        } else if (auto key = GamepadToImGuiKey(code); key != ImGuiKey_None) {
                            io.AddKeyEvent(key, down);
                        }
                        break;
                    default:
                        break;
                    }
                }
                else if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    // m32f: the left stick navigates like the d-pad. Edge-
                    // triggered at ±0.5 with release at the same threshold —
                    // ImGui's nav repeat takes over while a direction is held.
                    auto* th = static_cast<RE::ThumbstickEvent*>(e);
                    if (th->IsLeft()) {
                        auto edge = [&](int a_i, bool a_on, ImGuiKey a_key) {
                            if (g_stickNav[a_i] != a_on) {
                                g_stickNav[a_i] = a_on;
                                io.AddKeyEvent(a_key, a_on);
                            }
                        };
                        edge(0, th->yValue > 0.5f, ImGuiKey_GamepadDpadUp);
                        edge(1, th->yValue < -0.5f, ImGuiKey_GamepadDpadDown);
                        edge(2, th->xValue < -0.5f, ImGuiKey_GamepadDpadLeft);
                        edge(3, th->xValue > 0.5f, ImGuiKey_GamepadDpadRight);
                    }
                }
                // char events: swallowed silently
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
    ReadConfig();  // m24c: MCM Helper flushes iMenuStyle on ITS close — read
                   // fresh at every open so the skin dropdown takes effect now
    EnsurePouchRef();     // m27: gems present in the pouch before the snapshot
    RouteGemsToPouch();
    if (g_needSeedDiscoveries) { SeedDiscoveries(); g_needSeedDiscoveries = false; }
    CheckGemDiscoveries();  // m37: study newly-acquired gem families
    g_menu.selBase = 0;  // fresh open: no remembered selection
    g_menu.selUid = 0;
    g_menu.selItem = 0;
    g_menu.selSlot = -1;
    BuildMenuSnapshot();
    g_menu.wantClose = false;
    g_menu.station = a_station;
    menuhook::g_shoutDownSeen = false;  // m23c: opening press's release must not close
    for (bool& s : menuhook::g_stickNav) {
        s = false;  // m32f: no stuck stick directions from last open
    }
    menuhook::g_cursorInit = true;      // m23c: seed ImGui's cursor pos this frame
    menuhook::g_destroyArm = 0;
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

// m36h: support gems as rare boss loot (DESIGN §5 "not found before ~level 15").
// On a player boss/dragon kill past iSupportMinLevel, a small chance drops one
// random support gem (tier I) on the corpse. Hand-placed famous locations remain
// the guaranteed per-type source; this is the repeatable RNG backstop.
void RollBossSupportGem(RE::Actor* a_victim) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (g_supportGems.empty() || g_supportDropChance <= 0.0f || !a_victim || !player ||
        player->GetLevel() < g_supportMinLevel) {
        return;
    }
    if (std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) >= g_supportDropChance) {
        return;
    }
    const auto& rg = g_gems[g_supportGems[
        std::uniform_int_distribution<std::size_t>(0, g_supportGems.size() - 1)(g_rng)]];
    if (auto* item = rg.items[0]) {  // support gems drop at tier I
        a_victim->AddObjectToContainer(item, nullptr, 1, nullptr);
        spdlog::info("[loot] BOSS support gem '{}' I on {:08X}", rg.def->gid, a_victim->GetFormID());
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
                     GemName(g_gems[gemIdx]), meo::kRoman[level - 1], base->GetName());
    }
}

// ── m19: enemies spawn wearing socketed gear (DESIGN §3 post-strip economy) ──
// Archetype from the actor's race keyword / dominant base skills — picks which
// themed pool its gem rolls from.
Arch DetectArchetype(RE::Actor* a_actor) {
    if (g_kwUndead && a_actor->GetRace() && a_actor->GetRace()->HasKeyword(g_kwUndead)) {
        return Arch::kUndead;
    }
    auto* avo = a_actor->AsActorValueOwner();
    if (!avo) {
        return Arch::kWarrior;
    }
    auto av = [&](RE::ActorValue v) { return avo->GetBaseActorValue(v); };
    using AV = RE::ActorValue;
    const float mage = std::max({ av(AV::kDestruction), av(AV::kConjuration),
                                  av(AV::kIllusion), av(AV::kAlteration), av(AV::kRestoration) });
    const float rogue = std::max({ av(AV::kSneak), av(AV::kLightArmor), av(AV::kLockpicking) });
    const float warrior = std::max({ av(AV::kOneHanded), av(AV::kTwoHanded),
                                     av(AV::kHeavyArmor), av(AV::kArchery), av(AV::kBlock) });
    if (mage >= rogue && mage >= warrior) {
        return Arch::kMage;
    }
    return rogue > warrior ? Arch::kRogue : Arch::kWarrior;
}

// Deterministic per-reference roll (same discipline as world weapons): a given
// NPC decides once, forever. The gem is LIVE on the enemy (ApplyWornAbility) —
// you fight the effect; at death it converts to a lootable loose gem (below).
// ── m23: loot conversion (marth: covered enchants CONVERT to socketed) ──
// Any actor-held item in the conversion table is swapped for its
// unenchanted base carrying the matching family's gem — level I/II rolled
// with the same fGemLevel2Chance as loose drops, deterministic per
// holder+item. Instance creation uses the proven gem-return flow
// (PlaceObjectAtMe -> stamp the engine-created extraList -> PickUpObject);
// ExtraDataList has no plugin-side constructor. Non-actor containers are
// skipped ON PURPOSE: their items convert the moment they land on an
// actor (looting, purchase, pickup). Idempotent by construction: the
// converted base is not in the table, so a second sweep finds nothing.
RE::ExtraDataList* FindWornXListFor(RE::Actor* a_actor, RE::TESBoundObject* a_base) {
    auto* changes = a_actor->GetInventoryChanges();
    if (!changes || !changes->entryList) {
        return nullptr;
    }
    for (auto* entry : *changes->entryList) {
        if (!entry || entry->object != a_base || !entry->extraLists) {
            continue;
        }
        for (auto* xl : *entry->extraLists) {
            if (xl && IsWornXList(xl)) {
                return xl;
            }
        }
    }
    return nullptr;
}

// m26 (marth's rulings 2026-07-10): pre-MEO PLAYER-MADE enchants convert on
// load like loot. The item already IS the right base, so no remove/re-add —
// strip the old enchant extras and stamp matching family gems into the same
// instance (slots up to capacity). ALL GEMS LEVEL I, no magnitude matching —
// "that's the cost of adding mid-save" (his call: fairness over nostalgia).
// m27b (the helmet, finally named): lists RANK-TIER their fortify MGEFs
// ('Fortify Magicka (Rank II)' is a separate record from the catalog's
// fortifymagicka reference), so pointer identity misses kin effects. Two
// MGEFs with the same mechanical signature are the same gameplay effect —
// the installer's rule, mirrored here.
bool SameEffectSig(const RE::EffectSetting* a_a, const RE::EffectSetting* a_b) {
    using F = RE::EffectSetting::EffectSettingData::Flag;
    return a_a && a_b &&
           a_a->data.archetype == a_b->data.archetype &&
           a_a->data.primaryAV == a_b->data.primaryAV &&
           a_a->data.secondaryAV == a_b->data.secondaryAV &&
           a_a->data.resistVariable == a_b->data.resistVariable &&
           a_a->data.flags.all(F::kDetrimental) == a_b->data.flags.all(F::kDetrimental) &&
           a_a->data.flags.all(F::kHostile) == a_b->data.flags.all(F::kHostile);
}

int ConvertInstanceEnchant(RE::Actor* a_owner, RE::TESBoundObject* a_base,
                           RE::ExtraDataList* a_xList) {
    auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>();
    auto* ench = xEnch ? xEnch->enchantment : nullptr;
    if (!ench) {
        return 0;
    }
    const bool armor = a_base->Is(RE::FormType::Armor);
    const bool eligible = armor ? IsSocketableArmorBase(a_base->As<RE::TESObjectARMO>())
                                : IsSocketableWeaponBase(a_base->As<RE::TESObjectWEAP>());
    if (!eligible) {  // m32c: boots etc. can't socket — converting would strand them
        spdlog::info("[convert-miss] '{}' base {:08X} — not socketable gear, "
                     "player enchant left alone", a_base->GetName(), a_base->GetFormID());
        return 0;
    }
    const int  cap = SocketCapacity(a_base);
    std::vector<int> picks;
    for (auto* eff : ench->effects) {
        if (!eff || !eff->baseEffect) {
            continue;
        }
        int found = -1;
        for (std::size_t i = 0; i < g_gems.size(); ++i) {
            if (g_gems[i].mgef == eff->baseEffect && g_gems[i].def->isArmor == armor) {
                found = static_cast<int>(i);
                break;
            }
        }
        if (found < 0) {  // ranked/kin variant — signature identity
            for (std::size_t i = 0; i < g_gems.size(); ++i) {
                if (g_gems[i].mgef && g_gems[i].def->isArmor == armor &&
                    SameEffectSig(g_gems[i].mgef, eff->baseEffect)) {
                    found = static_cast<int>(i);
                    break;
                }
            }
        }
        if (found < 0) {
            spdlog::info("[convert]   dropping non-gem effect '{}' ({:08X}) mag={:.1f} — no family",
                         eff->baseEffect->GetName(), eff->baseEffect->GetFormID(),
                         eff->effectItem.magnitude);
            continue;
        }
        bool dup = false;
        for (int p : picks) {
            dup = dup || p == found;
        }
        if (!dup && static_cast<int>(picks.size()) < cap) {
            picks.push_back(found);
        } else if (!dup) {
            spdlog::info("[convert]   dropping overflow effect '{}' — out of sockets (cap {})",
                         eff->baseEffect->GetName(), cap);
        }
    }
    if (picks.empty()) {
        spdlog::info("[convert-miss] '{}' base {:08X} — instance enchant matches no gem "
                     "family, left alone; effects:", a_base->GetName(), a_base->GetFormID());
        for (auto* eff : ench->effects) {
            if (eff && eff->baseEffect) {
                // names the exact MGEF a new family would need (m26c)
                spdlog::info("[convert-miss]   '{}' ({:08X}) mag={:.1f} dur={:.0f}",
                             eff->baseEffect->GetName(), eff->baseEffect->GetFormID(),
                             eff->effectItem.magnitude,
                             static_cast<float>(eff->effectItem.duration));
            }
        }
        return 0;
    }
    const bool worn = IsWornXList(a_xList);
    a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
    a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);  // forge rename dies with it
    std::string what;
    for (std::size_t s = 0; s < picks.size(); ++s) {
        StampInstance(a_base, a_xList, picks[s], 1, static_cast<std::uint8_t>(s));
        if (!what.empty()) {
            what += " + ";
        }
        what += std::format("{} I", GemName(g_gems[picks[s]]));
    }
    if (worn && a_owner) {
        EquipCycleWorn(a_owner, a_base, a_xList);  // old ability out, gem live
    }
    spdlog::info("[convert] {:08X} player enchant on '{}' -> {}",
                 a_base->GetFormID(), a_base->GetName(), what);
    return 1;
}

// m37 (conversion blocker: vendor barter stock never converted): this used to
// bail on any non-actor holder, so StockVendorGems's sweep of the merchant
// CONTAINER (a chest ref, not an actor) has been a silent no-op since m23 —
// which is exactly where a low-level player meets "of Major Wielding / Minor
// Alteration / the Major Knight" armor. Now a container holder is swept too,
// via the engine's own flow (PlaceObjectAtMe -> stamp -> AddObjectToContainer
// with fromRefr, which consumes the temp ref like Papyrus AddItem(ObjectReference)).
int ConvertInventory(RE::TESObjectREFR* a_holder) {
    auto* actor = a_holder ? a_holder->As<RE::Actor>() : nullptr;
    if (!a_holder || g_convert.empty()) {
        return 0;
    }
    struct Hit {
        RE::TESBoundObject* old;
        const ConvTarget*   tgt;
        std::int32_t        count;
        bool                worn;
        bool                left;
    };
    std::vector<Hit> hits;
    for (auto& [obj, data] : a_holder->GetInventory()) {
        auto it = g_convert.find(obj->GetFormID());
        if (it == g_convert.end() || data.first <= 0) {
            continue;
        }
        bool worn = false, left = false;
        if (data.second && data.second->IsWorn()) {
            worn = true;
            if (data.second->extraLists) {
                for (auto* xl : *data.second->extraLists) {
                    if (xl && xl->HasType(RE::ExtraDataType::kWornLeft)) {
                        left = true;
                    }
                }
            }
        }
        hits.push_back({ obj, &it->second, data.first, worn, left });
    }
    int converted = 0;
    // m25d (marth's helmet/cuirass): when the PLAYER sweep misses enchanted
    // gear, say exactly what and why — an enchanted BASE not in the table is
    // an installer question; an INSTANCE enchant (on the copy, not the
    // record) is the player-enchant signature and skipped by design.
    if (actor && actor->IsPlayerRef()) {
        for (auto& [obj, data] : actor->GetInventory()) {
            if (data.first <= 0 || g_convert.contains(obj->GetFormID())) {
                continue;
            }
            RE::EnchantmentItem* baseEnch = nullptr;
            if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                baseEnch = w->formEnchanting;
            } else if (auto* ar = obj->As<RE::TESObjectARMO>()) {
                baseEnch = ar->formEnchanting;
            } else {
                continue;
            }
            if (baseEnch) {
                spdlog::info("[convert-miss] '{}' base {:08X} ench {:08X} — enchanted base "
                             "not in the conversion table",
                             obj->GetName(), obj->GetFormID(), baseEnch->GetFormID());
                continue;
            }
            if (!data.second || !data.second->extraLists) {
                continue;
            }
            for (auto* xl : *data.second->extraLists) {
                if (!xl || !xl->HasType(RE::ExtraDataType::kEnchantment)) {
                    continue;
                }
                auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                bool  ours = false;
                for (int s = 0; xid && s < kMaxSockets; ++s) {
                    if (g_sockets.contains(MakeKey(obj->GetFormID(), xid->uniqueID,
                                                   static_cast<std::uint8_t>(s)))) {
                        ours = true;
                        break;
                    }
                }
                if (!ours) {
                    converted += ConvertInstanceEnchant(actor, obj, xl);  // m26: marth's ruling
                    break;
                }
            }
        }
    }
    const std::uint32_t seed = HashU32(a_holder->GetFormID() ^ 0x4D454F43u);  // 'MEOC'
    const std::uint32_t l2cut = static_cast<std::uint32_t>(g_gemLevel2Chance * 10000.0f);
    for (const auto& hit : hits) {
        a_holder->RemoveItem(hit.old, hit.count, RE::ITEM_REMOVE_REASON::kRemove,
                             nullptr, nullptr);
        for (std::int32_t i = 0; i < hit.count; ++i) {
            auto ref = a_holder->PlaceObjectAtMe(hit.tgt->base, false);
            if (!ref) {
                spdlog::error("[convert] PlaceObjectAtMe failed for '{}' — plain base given",
                              hit.tgt->base->GetName());
                a_holder->AddObjectToContainer(hit.tgt->base, nullptr, 1, nullptr);
                continue;
            }
            auto& xl = ref->extraList;
            // m34c (marth: converted BANDIT loot flagged as owned/theft):
            // stamp ownership ONLY for the player's own conversion — m17b
            // prevents the PLAYER's place-and-pickup near guards from reading
            // as theft. Enemy loot must stay unowned (vanilla free corpse
            // loot); vendor stock is gated by the merchant container, not
            // per-item ownership; and here the ACTOR, not the player, does the
            // pickup, so there is no player-theft to prevent. Baking the
            // actor's ownership was what made a bandit's converted drop
            // read as stolen.
            if (actor && actor->IsPlayerRef()) {
                xl.SetOwner(actor->GetActorBase());
            }
            const std::uint32_t hi =
                HashU32(seed ^ hit.old->GetFormID() ^ static_cast<std::uint32_t>(i));
            const int level = (hi % 10000) < l2cut ? 2 : 1;
            StampInstance(hit.tgt->base, &xl, hit.tgt->gemIdx, level);
            if (actor) {
                actor->PickUpObject(ref.get(), 1, false, false);
            } else {
                // container holder (vendor chest): move the stamped instance in,
                // fromRefr = the temp world ref, which the engine then consumes.
                a_holder->AddObjectToContainer(hit.tgt->base, &xl, 1, ref.get());
            }
            ++converted;
        }
        // The list picked this enemy to fight with an enchanted weapon; the
        // converted gem stays worn and ACTIVE (marth's ruling).
        if (actor && hit.worn) {
            RE::ActorEquipManager::GetSingleton()->EquipObject(
                actor, hit.tgt->base, nullptr, 1, nullptr, false, false, false, true);
            if (auto* wxl = FindWornXListFor(actor, hit.tgt->base)) {
                ApplyWornAbility(actor, hit.tgt->base, wxl, hit.left);
            }
        }
        spdlog::info("[convert] {:08X} '{}'{}: '{}' x{} -> '{}' + {} gem",
                     a_holder->GetFormID(), a_holder->GetName(), actor ? "" : " (container)",
                     hit.old->GetName(), hit.count, hit.tgt->base->GetName(),
                     GemName(g_gems[hit.tgt->gemIdx]));
    }
    if (!actor && converted > 0) {  // m37: vendor/container sweeps self-diagnose now
        spdlog::info("[convert] container {:08X} '{}': {} item(s) converted",
                     a_holder->GetFormID(), a_holder->GetName(), converted);
    }
    return converted;
}

// m26 (marth: an enchanted name floating over a world item until pickup
// "is jarring"): loose world refs of convertible generics swap IN PLACE at
// cell attach — but only disposable clutter. Persistent and quest-aliased
// refs are exactly the ones scripts point at, so they keep converting on
// pickup (ContainerSink) instead of being replace-and-deleted here.
void ConvertWorldRef(RE::TESObjectREFR* a_ref) {
    if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted()) {
        return;
    }
    auto* base = a_ref->GetBaseObject();
    if (!base) {
        return;
    }
    auto it = g_convert.find(base->GetFormID());
    if (it == g_convert.end()) {
        return;
    }
    // m36f (Fable): these three deferrals are BY DESIGN — the item still converts,
    // just on pickup, not on the shelf. Logging the reason makes "it's not
    // converting!" reports self-diagnosing (the item is convertible, deferred).
    if ((a_ref->formFlags & RE::TESObjectREFR::RecordFlags::kPersistent) != 0) {
        spdlog::info("[convert-defer] '{}' {:08X} persistent world ref — converts on pickup",
                     base->GetName(), a_ref->GetFormID());
        return;  // scripts/quests may hold this exact ref
    }
    if (a_ref->extraList.GetCount() > 1) {
        spdlog::info("[convert-defer] '{}' {:08X} stacked pile x{} — converts per-unit on pickup",
                     base->GetName(), a_ref->GetFormID(), a_ref->extraList.GetCount());
        return;  // m35: a stacked world pile — PlaceObjectAtMe makes only one,
                 // so converting in place would destroy the rest. Let it convert
                 // per-unit on pickup via the container sink instead.
    }
    if (a_ref->extraList.HasType(RE::ExtraDataType::kAliasInstanceArray)) {
        spdlog::info("[convert-defer] '{}' {:08X} quest-aliased — converts on pickup",
                     base->GetName(), a_ref->GetFormID());
        return;  // quest-aliased — same caution
    }
    auto newRef = a_ref->PlaceObjectAtMe(it->second.base, false);
    if (!newRef) {
        return;
    }
    if (auto* owner = a_ref->GetOwner()) {
        newRef->extraList.SetOwner(owner);  // pickup keeps the same theft semantics
    }
    const std::uint32_t h = HashU32(a_ref->GetFormID() ^ 0x4D454F57u);
    const int level =
        (h % 10000) < static_cast<std::uint32_t>(g_gemLevel2Chance * 10000.0f) ? 2 : 1;
    StampInstance(it->second.base, &newRef->extraList, it->second.gemIdx, level);
    newRef->data.angle = a_ref->data.angle;  // keep the shelf pose
    spdlog::info("[convert] world ref {:08X} '{}' -> '{}' + {} gem",
                 a_ref->GetFormID(), base->GetName(), it->second.base->GetName(),
                 GemName(g_gems[it->second.gemIdx]));
    a_ref->Disable();
    a_ref->SetDelete(true);
}

void MaybeStampNPCGear(RE::Actor* a_actor) {
    if (!a_actor || g_npcSocketChance <= 0.0f || !g_kwNPC || a_actor->IsPlayerRef() ||
        a_actor->IsPlayerTeammate() || a_actor->IsDead()) {
        return;
    }
    auto* race = a_actor->GetRace();
    if (!race || !race->HasKeyword(g_kwNPC)) {
        return;  // humanoids only — creatures don't wear gear
    }
    // m19e: ENEMY classes only (marth) — civilians are Unaggressive (0);
    // bandits/Forsworn/raiders are Aggressive+ (>=1). Base AV, not current.
    if (auto* avo = a_actor->AsActorValueOwner();
        !avo || avo->GetBaseActorValue(RE::ActorValue::kAggression) < 1.0f) {
        return;
    }
    const std::uint32_t h = HashU32(a_actor->GetFormID() ^ 0x4D454F32u);  // 'MEO2'
    if ((h % 10000) / 10000.0f >= g_npcSocketChance) {
        return;
    }
    auto* changes = a_actor->GetInventoryChanges();
    if (!changes || !changes->entryList) {
        return;
    }
    struct Cand { RE::TESBoundObject* base; RE::ExtraDataList* xl; bool armor; bool left; };
    std::vector<Cand> cands;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists) {
            continue;
        }
        const bool isW = entry->object->Is(RE::FormType::Weapon);
        const bool isA = entry->object->Is(RE::FormType::Armor);
        if (!isW && !isA) {
            continue;
        }
        for (auto* xl : *entry->extraLists) {
            if (!xl || !IsWornXList(xl)) {
                continue;
            }
            if (auto* xid = xl->GetByType<RE::ExtraUniqueID>()) {
                for (int s = 0; s < kMaxSockets; ++s) {
                    if (g_sockets.contains(MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                                   static_cast<std::uint8_t>(s)))) {
                        return;  // already blessed (re-attach after a save)
                    }
                }
            }
            if (xl->HasType(RE::ExtraDataType::kEnchantment)) {
                continue;  // foreign enchant
            }
            if (isW && !IsSocketableWeaponBase(entry->object->As<RE::TESObjectWEAP>())) {
                continue;
            }
            if (isA && !IsSocketableArmorBase(entry->object->As<RE::TESObjectARMO>())) {
                continue;
            }
            cands.push_back({ entry->object, xl, isA,
                              xl->HasType(RE::ExtraDataType::kWornLeft) });
        }
    }
    if (cands.empty()) {
        return;
    }
    const auto& c = cands[HashU32(h ^ 0xC0FFEEu) % cands.size()];
    const int   arch = static_cast<int>(DetectArchetype(a_actor));
    const auto& pool = g_npcPool[arch][c.armor ? 1 : 0];
    if (pool.empty()) {
        return;
    }
    const int gemIdx = pool[HashU32(h ^ 0xBEEF5A5Au) % pool.size()];
    const std::uint32_t l2cut = static_cast<std::uint32_t>(g_gemLevel2Chance * 10000.0f);
    const int level = (HashU32(h ^ 0x22222222u) % 10000) < l2cut ? 2 : 1;
    if (StampInstance(c.base, c.xl, gemIdx, level)) {
        ApplyWornAbility(a_actor, c.base, c.xl, c.left);  // live on the enemy
        static constexpr const char* kArchNames[] = { "warrior", "mage", "rogue", "undead" };
        spdlog::info("[npc] {:08X} '{}' ({}) spawns with {} {} on {}", a_actor->GetFormID(),
                     a_actor->GetName(), kArchNames[arch], GemName(g_gems[gemIdx]),
                     meo::kRoman[level - 1], c.base->GetName());
    }
}

// m19: enemy gems STAY IN THE GEAR (marth 2026-07-09) — the corpse's socketed
// item is itself the loot; loose corpse gems (fGemDropChance) are a separate
// pool. That makes the §1 uid-rewrite trap unavoidable: any container
// transfer (corpse->player loot, buying, chests) REWRITES ExtraUniqueID and
// would orphan the socket record. This re-key keeps the record alive: on a
// transfer of a base we have records for, find the arriving orphan xList
// (enchanted, record-less) in the new container and the stranded record
// (uid now in neither container), and move the record to the new uid. The
// event's uniqueID field is used as a hint for BOTH possible semantics
// (old-side or new-side uid — unproven which; the [rekey] log line will
// settle it in-game). Ambiguous multi-instance transfers log and skip
// (no corruption — worst case one orphaned instance, the pre-m19 status quo).
void RekeyTransferredSockets(RE::FormID a_base, RE::FormID a_oldC, RE::FormID a_newC,
                             std::uint16_t a_evUid) {
    auto* newRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_newC);
    auto* oldRef = a_oldC ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_oldC) : nullptr;
    if (!newRef) {
        return;
    }
    using UidXl = std::pair<std::uint16_t, RE::ExtraDataList*>;
    auto collect = [a_base](RE::TESObjectREFR* r, std::vector<UidXl>& out) {
        auto* ch = r ? r->GetInventoryChanges() : nullptr;
        if (!ch || !ch->entryList) {
            return;
        }
        for (auto* e : *ch->entryList) {
            if (!e || !e->object || e->object->GetFormID() != a_base || !e->extraLists) {
                continue;
            }
            for (auto* xl : *e->extraLists) {
                if (xl) {
                    if (auto* xid = xl->GetByType<RE::ExtraUniqueID>()) {
                        out.emplace_back(xid->uniqueID, xl);
                    }
                }
            }
        }
    };
    std::vector<UidXl> inNew, inOld;
    collect(newRef, inNew);
    collect(oldRef, inOld);
    auto hasRecord = [a_base](std::uint16_t uid) {
        for (int s = 0; s < kMaxSockets; ++s) {
            if (g_sockets.contains(MakeKey(a_base, uid, static_cast<std::uint8_t>(s)))) {
                return true;
            }
        }
        return false;
    };
    // Arriving orphans: enchanted instances in the NEW container with no record.
    std::vector<UidXl> orphans;
    for (auto& [uid, xl] : inNew) {
        if (!hasRecord(uid) && xl->HasType(RE::ExtraDataType::kEnchantment)) {
            orphans.emplace_back(uid, xl);
        }
    }
    if (orphans.empty()) {
        return;
    }
    // Stranded records: a record uid now present in NEITHER container.
    auto uidPresent = [&](std::uint16_t uid) {
        for (auto& [u, xl] : inNew) { if (u == uid) return true; }
        for (auto& [u, xl] : inOld) { if (u == uid) return true; }
        return false;
    };
    std::vector<std::uint16_t> stranded;
    for (auto& [key, rec] : g_sockets) {
        if (static_cast<RE::FormID>(key >> 24) != a_base) {
            continue;
        }
        const auto uid = static_cast<std::uint16_t>((key >> 8) & 0xFFFF);
        if (!uidPresent(uid) &&
            std::find(stranded.begin(), stranded.end(), uid) == stranded.end()) {
            stranded.push_back(uid);
        }
    }
    if (stranded.empty()) {
        return;
    }
    std::uint16_t fromUid = 0;
    if (std::find(stranded.begin(), stranded.end(), a_evUid) != stranded.end()) {
        fromUid = a_evUid;  // event uid named the OLD (stranded) side
    } else if (stranded.size() == 1) {
        fromUid = stranded[0];
    } else {
        spdlog::warn("[rekey] {:08X}: ambiguous ({} stranded, evUid={}) — skipped",
                     a_base, stranded.size(), a_evUid);
        return;
    }
    RE::ExtraDataList* toXl = nullptr;
    std::uint16_t      toUid = 0;
    for (auto& [uid, xl] : orphans) {
        if (uid == a_evUid) {  // event uid named the NEW (arriving) side
            toXl = xl;
            toUid = uid;
        }
    }
    if (!toXl && orphans.size() == 1) {
        toXl = orphans[0].second;
        toUid = orphans[0].first;
    }
    if (!toXl || toUid == fromUid) {
        if (!toXl) {
            spdlog::warn("[rekey] {:08X}: ambiguous ({} orphans, evUid={}) — skipped",
                         a_base, orphans.size(), a_evUid);
        }
        return;
    }
    for (int s = 0; s < kMaxSockets; ++s) {
        auto it = g_sockets.find(MakeKey(a_base, fromUid, static_cast<std::uint8_t>(s)));
        if (it == g_sockets.end()) {
            continue;
        }
        g_sockets[MakeKey(a_base, toUid, static_cast<std::uint8_t>(s))] = std::move(it->second);
        g_sockets.erase(it);
    }
    spdlog::info("[rekey] {:08X}: uid {} -> {} (evUid={}; {} orphan(s), {} stranded)",
                 a_base, fromUid, toUid, a_evUid, orphans.size(), stranded.size());
}

// ── m19b: vendors sell loose gems (DESIGN §3 post-strip economy) ──────
// Loose gem MISC items are plain stackables — container-transfer-safe (no §1
// uid concerns). Socketed vendor STOCK is deferred until the container-
// transfer re-key (TESContainerChangedEvent) is proven. Deterministic per
// vendor per game day: per stock item, fVendorGemChance of adding one gem
// (tier-weighted corpse pool), capped at 3; skipped while any MEO gem is
// still in stock (no re-roll stacking on menu reopen).
std::unordered_map<RE::FormID, std::uint32_t> g_vendorStockedDay;  // session-only

void StockVendorGems() {
    if (g_vendorGemChance <= 0.0f || g_corpseGems.empty()) {
        return;
    }
    auto* mtm = RE::MenuTopicManager::GetSingleton();
    auto  speaker = mtm ? mtm->speaker.get() : RE::NiPointer<RE::TESObjectREFR>{};
    auto* vendor = speaker ? speaker->As<RE::Actor>() : nullptr;
    if (!vendor) {
        return;
    }
    RE::TESObjectREFR* target = vendor;  // fallback: vendors sell their own inventory
    if (auto* base = vendor->GetActorBase()) {
        for (auto& fr : base->factions) {
            if (fr.faction && fr.faction->IsVendor() &&
                fr.faction->vendorData.merchantContainer) {
                target = fr.faction->vendorData.merchantContainer;
                break;
            }
        }
    }
    // m23: vendor stock is LVLI-generated in place (no container events),
    // so convert enchanted generics here, at dialogue time.
    ConvertInventory(target);
    int  itemCount = 0;
    bool hasGem = false;
    for (auto& [obj, data] : target->GetInventory()) {
        if (data.first > 0) {
            ++itemCount;
            if (obj && g_gemByItem.contains(obj->GetFormID())) {
                hasGem = true;
            }
        }
    }
    const auto* cal = RE::Calendar::GetSingleton();
    const std::uint32_t day = cal ? static_cast<std::uint32_t>(cal->GetDaysPassed()) : 0;
    // One stocking per vendor per game day: the session map stops a buy-out ->
    // reopen from re-adding the same deterministic picks; the in-stock check
    // covers fresh sessions mid-cycle.
    if (auto it = g_vendorStockedDay.find(target->GetFormID());
        it != g_vendorStockedDay.end() && it->second == day) {
        return;
    }
    g_vendorStockedDay[target->GetFormID()] = day;
    if (hasGem || itemCount == 0) {
        return;  // this restock cycle already has gem stock
    }
    const std::uint32_t h = HashU32(target->GetFormID() ^ (day * 0x9E3779B9u) ^ 0x4D454F33u);
    const std::uint32_t cut = static_cast<std::uint32_t>(g_vendorGemChance * 10000.0f);
    const std::uint32_t l2cut = static_cast<std::uint32_t>(g_gemLevel2Chance * 10000.0f);
    int added = 0;
    for (int i = 0; i < itemCount && added < 3; ++i) {
        if (HashU32(h ^ static_cast<std::uint32_t>(i)) % 10000 >= cut) {
            continue;
        }
        const int gemIdx = g_corpseGems[HashU32(h ^ 0x900Du ^ static_cast<std::uint32_t>(i)) %
                                        g_corpseGems.size()];
        const int level = (HashU32(h ^ 0x33333333u ^ static_cast<std::uint32_t>(i)) % 10000) <
                          l2cut ? 2 : 1;
        if (auto* gemForm = g_gems[gemIdx].items[level - 1]) {
            target->AddObjectToContainer(gemForm, nullptr, 1, nullptr);
            ++added;
            spdlog::info("[vendor] {:08X} stocks {} {}", target->GetFormID(),
                         GemName(g_gems[gemIdx]), meo::kRoman[level - 1]);
        }
    }
}

// m36i: hand-placed support gems — one guaranteed copy per type at a famous,
// NON-respawning container (Fable-verified Skyrim.esm refs). Dropped once per
// save via the v10 co-save bitmask. Master-00 RefIDs (load-order-stable); a
// null/disabled/deleted ref (a heavy overhaul relocated or emptied it) degrades
// to a silent no-op — those players still get support gems from boss loot.
struct HandPlaced { RE::FormID ref; const char* gid; std::uint8_t bit; };
inline constexpr HandPlaced kHandPlaced[] = {
    { 0x000C8996, "focus",   0x01 },  // Avanchnzel Boilery boss chest (Dwemer / the Lexicon)
    { 0x000CE735, "conduit", 0x02 },  // The Midden — Atronach Forge Offering Box (transmutation; persistent ref)
    { 0x00026B6D, "echo",    0x04 },  // Ustengrav Depths boss chest (Jurgen Windcaller — the Voice)
};

// Place the hand-placed gem into a container ref if it now resolves and hasn't
// been placed. a_onlyRef != 0 restricts to that ref (cell-attach); 0 tries all
// currently-resolvable refs (post-load — catches the persistent Offering Box).
void TryPlaceHandPlaced(RE::FormID a_onlyRef = 0) {
    for (const auto& hp : kHandPlaced) {
        if ((g_handPlacedMask & hp.bit) || (a_onlyRef && a_onlyRef != hp.ref)) {
            continue;
        }
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(hp.ref);
        if (!ref || ref->IsDisabled() || ref->IsDeleted()) {
            continue;  // cell not loaded yet, or overhaul removed it — retry later / skip
        }
        auto gi = g_gemByGid.find(hp.gid);
        if (gi == g_gemByGid.end() || !g_gems[gi->second].items[0]) {
            continue;
        }
        ref->AddObjectToContainer(g_gems[gi->second].items[0], nullptr, 1, nullptr);  // tier I
        g_handPlacedMask |= hp.bit;
        spdlog::info("[handplaced] '{}' I placed in container {:08X}", hp.gid, hp.ref);
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
        // m36i: a hand-placed support-gem container just loaded — drop its gem
        // once (the boss chests are temporary refs, resolvable only now).
        const RE::FormID rid = a_event->reference->GetFormID();
        for (const auto& hp : kHandPlaced) {
            if (rid == hp.ref && !(g_handPlacedMask & hp.bit)) {
                SKSE::GetTaskInterface()->AddTask([rid]() { TryPlaceHandPlaced(rid); });
                break;
            }
        }
        if (auto* bo = a_event->reference->GetBaseObject();
            bo && (bo->Is(RE::FormType::Weapon) || bo->Is(RE::FormType::Armor))) {
            const RE::ObjectRefHandle handle = a_event->reference->GetHandle();
            SKSE::GetTaskInterface()->AddTask([handle]() {
                if (auto ref = handle.get()) {
                    auto* b = ref->GetBaseObject();
                    if (b && g_convert.contains(b->GetFormID())) {
                        ConvertWorldRef(ref.get());  // m26: no jarring names on shelves
                    } else if (b && b->Is(RE::FormType::Weapon)) {
                        MaybeStampWorldWeapon(ref.get());
                    }
                }
            });
        } else if (a_event->reference->GetBaseObject() &&
                   a_event->reference->GetBaseObject()->Is(RE::FormType::NPC)) {
            // m19: NPC refs roll a themed worn socket the moment they attach.
            const RE::ObjectRefHandle handle = a_event->reference->GetHandle();
            SKSE::GetTaskInterface()->AddTask([handle]() {
                if (auto ref = handle.get()) {
                    if (auto* actor = ref->As<RE::Actor>()) {
                        ConvertInventory(actor);
                        MaybeStampNPCGear(actor);
                    }
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
        const bool                isBoss = mult > 1.0f;  // m36h: boss/dragon → support-gem roll
        const RE::ObjectRefHandle victim = a_event->actorDying->GetHandle();
        const RE::ObjectRefHandle killerHandle = a_event->actorKiller->GetHandle();
        const float               xp = g_xpPerKill * mult;
        SKSE::GetTaskInterface()->AddTask([victim, killerHandle, xp, fromPlayer, isBoss]() {
            if (auto k = killerHandle.get()) {
                if (auto* killerActor = k->As<RE::Actor>()) {
                    AwardKillXP(killerActor, xp);
                }
            }
            if (fromPlayer) {  // corpse gems stay player-kill only
                if (auto ref = victim.get()) {
                    if (auto* v = ref->As<RE::Actor>()) {
                        RollCorpseGem(v);
                        if (isBoss) {  // m36h: rare support gem from boss/dragon loot
                            RollBossSupportGem(v);
                        }
                    }
                }
            }
        });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// m36: Echo (armor half) — follower-share. If the player wears a linked
// Echo-armor gem, re-cast that gem's effect (× the Echo tier fraction) on every
// current follower for a short duration, every heartbeat. SELF-EXPIRING: no
// permanent abilities are added, so if the pairing breaks (unlinked/unequipped)
// or MEO is removed, the effect simply lapses within its duration — nothing to
// track, nothing to leak. Uses ONE persistent ESP spell (MEO_EchoShare, real
// FormID) whose single effect is rewritten each tick, so follower saves only
// ever reference a real form, never a runtime-created one.
void EchoFollowerShareTick() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !g_echoShareSpell || g_echoShareSpell->effects.size() == 0) {
        return;
    }
    const ResolvedGem* shareGem = nullptr;
    int   shareLvIdx = 0;
    float echoFrac = 0.0f;
    auto* changes = player->GetInventoryChanges();
    if (changes && changes->entryList) {
        for (auto* entry : *changes->entryList) {
            if (shareGem || !entry || !entry->object || !entry->object->Is(RE::FormType::Armor) ||
                !entry->extraLists) {
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
                const ResolvedGem* support = nullptr;
                const ResolvedGem* normal = nullptr;
                int                stier = 1, nlvl = 1;
                for (int s = 0; s < kMaxSockets; ++s) {
                    auto it = g_sockets.find(MakeKey(entry->object->GetFormID(), xid->uniqueID,
                                                     static_cast<std::uint8_t>(s)));
                    if (it == g_sockets.end()) {
                        continue;
                    }
                    auto gi = g_gemByGid.find(it->second.gid);
                    if (gi == g_gemByGid.end()) {
                        continue;
                    }
                    if (g_gems[gi->second].def->isSupport) {
                        support = &g_gems[gi->second];
                        stier = std::clamp<int>(it->second.level, 1, 3);
                    } else {
                        normal = &g_gems[gi->second];
                        nlvl = it->second.level;
                    }
                }
                if (support && support->def->supportType == meo::SupportType::kEcho && normal &&
                    normal->mgef) {
                    shareGem = normal;
                    shareLvIdx = std::clamp<int>(nlvl, 1, 5) - 1;
                    echoFrac = support->def->tierParam[stier - 1];
                }
                break;
            }
        }
    }
    // m36g: quiet logging — remember the last shared state and only log when it
    // changes, so combat/idle isn't a wall of identical 8s lines.
    static std::string s_lastGid;
    static int         s_lastN = -1;
    static int         s_lastMag = -1;
    if (!shareGem) {
        if (s_lastN > 0) {  // transition: was sharing, now not
            spdlog::info("[echo-share] ended — no linked Echo-armor");
        }
        s_lastGid.clear();
        s_lastN = -1;
        s_lastMag = -1;
        return;  // prior casts expire on their own (self-expiring)
    }
    // Rewrite the share spell's single effect to this gem's, scaled by tier.
    const float mag = GemBaseMag(shareGem->def, shareLvIdx) * g_magnitudeMult *
                      (1.0f + 0.08f * g_attuneRank) * GemPerkMult(shareGem->def) * echoFrac;
    auto* eff = g_echoShareSpell->effects[0];
    if (!eff) {
        return;
    }
    eff->baseEffect = shareGem->mgefLv[shareLvIdx];
    eff->effectItem.magnitude = mag;
    eff->effectItem.area = 0;
    eff->effectItem.duration = 12;  // seconds; > heartbeat so followers stay buffed
    auto* lists = RE::ProcessLists::GetSingleton();
    auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
    if (!lists || !caster) {
        return;
    }
    int shared = 0;
    for (auto& handle : lists->highActorHandles) {
        auto a = handle.get();
        if (!a || a.get() == player || !a->IsPlayerTeammate() || a->IsDead()) {
            continue;
        }
        // m36g: dispel any PRIOR share-cast on this follower before re-casting, so
        // the shared buff is always exactly 1× — never stacks, regardless of
        // whether the engine refreshes or duplicates same-spell effects. Cheap:
        // a follower has only a handful of active effects.
        if (auto* mt = a->AsMagicTarget()) {
            if (auto* elist = mt->GetActiveEffectList()) {
                for (auto* ae : *elist) {
                    if (ae && ae->spell == g_echoShareSpell) {
                        ae->Dispel(true);
                    }
                }
            }
        }
        caster->CastSpellImmediate(g_echoShareSpell, false, a.get(), 1.0f, false, 0.0f, player);
        ++shared;
    }
    const int magR = static_cast<int>(std::lround(mag));
    if (shareGem->def->gid != s_lastGid || shared != s_lastN || magR != s_lastMag) {
        spdlog::info("[echo-share] '{}' mag={:.1f} dur=12 → {} follower(s) (re-cast/8s, 1×-dispel)",
                     shareGem->def->gid, mag, shared);
        s_lastGid = shareGem->def->gid;
        s_lastN = shared;
        s_lastMag = magR;
    }
}

// Heartbeat: drives EchoFollowerShareTick on the main thread every 8s. Started
// once at data-load; harmless at the main menu (the tick no-ops with no player).
void StartEchoHeartbeat() {
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) {
        return;
    }
    std::thread([]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(8));
            SKSE::GetTaskInterface()->AddTask([]() { EchoFollowerShareTick(); });
        }
    }).detach();
}

// m36: Echo (weapon half) — a linked elemental gem's on-hit effect bursts in an
// area. The enchant `area` field does nothing on a weapon (contact delivery), so
// the AoE is delivered here: on a player weapon hit, re-cast that weapon's own
// enchantment on nearby hostiles within a tier-scaled radius. Casting applies
// magic (no weapon swing), so it cannot re-enter this hit event.
class HitSink : public RE::BSTEventSink<RE::TESHitEvent> {
public:
    static HitSink* GetSingleton() {
        static HitSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                          RE::BSTEventSource<RE::TESHitEvent>*) override {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_event || !player || !a_event->cause || a_event->cause.get() != player) {
            return RE::BSEventNotifyControl::kContinue;  // player weapon hits only (for now)
        }
        auto* victim = a_event->target ? a_event->target->As<RE::Actor>() : nullptr;
        if (!victim || victim->IsDead()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // Is the weapon that struck (a_event->source) Echo-linked to an elemental
        // gem? If so, capture its enchant to re-cast and the tier radius.
        RE::EnchantmentItem* ench = nullptr;
        float                radius = 0.0f;
        auto* changes = player->GetInventoryChanges();
        if (changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (ench || !entry || !entry->object ||
                    entry->object->GetFormID() != a_event->source ||
                    !entry->object->Is(RE::FormType::Weapon) || !entry->extraLists) {
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
                    const ResolvedGem* support = nullptr;
                    const ResolvedGem* normal = nullptr;
                    int                stier = 1;
                    for (int s = 0; s < kMaxSockets; ++s) {
                        auto it = g_sockets.find(MakeKey(a_event->source, xid->uniqueID,
                                                         static_cast<std::uint8_t>(s)));
                        if (it == g_sockets.end()) {
                            continue;
                        }
                        auto gi = g_gemByGid.find(it->second.gid);
                        if (gi == g_gemByGid.end()) {
                            continue;
                        }
                        if (g_gems[gi->second].def->isSupport) {
                            support = &g_gems[gi->second];
                            stier = std::clamp<int>(it->second.level, 1, 3);
                        } else {
                            normal = &g_gems[gi->second];
                        }
                    }
                    if (support && support->def->supportType == meo::SupportType::kEcho &&
                        normal && IsElementalGem(normal)) {
                        radius = support->def->tierParam[stier - 1] * 300.0f;
                        if (auto* xe = xl->GetByType<RE::ExtraEnchantment>()) {
                            ench = xe->enchantment;
                        }
                    }
                    break;
                }
            }
        }
        if (!ench || radius <= 0.0f) {
            return RE::BSEventNotifyControl::kContinue;  // not an Echo-linked weapon hit
        }
        const RE::NiPoint3     vpos = victim->GetPosition();
        const RE::ObjectRefHandle vh = victim->GetHandle();
        // Cast on nearby hostiles on the task queue (main thread, off the event).
        SKSE::GetTaskInterface()->AddTask([ench, radius, vpos, vh]() {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* lists = RE::ProcessLists::GetSingleton();
            auto* caster = pl ? pl->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
            if (!pl || !lists || !caster) {
                return;
            }
            auto vic = vh.get();
            int hit = 0;
            for (auto& handle : lists->highActorHandles) {
                auto a = handle.get();
                if (!a || a.get() == pl || (vic && a.get() == vic.get())) {
                    continue;
                }
                const float dist = a->GetPosition().GetDistance(vpos);
                if (dist > radius) {
                    continue;  // out of range — not logged (too noisy)
                }
                // Catch anyone hostile OR in combat — a guard fighting you reads
                // as in-combat even if the faction hostility check lags. Neutral,
                // non-fighting bystanders (IsInCombat=false) and your followers
                // are spared.
                if (a->IsPlayerTeammate() || a->IsDead() ||
                    !(a->IsHostileToActor(pl) || a->IsInCombat())) {
                    continue;
                }
                caster->CastSpellImmediate(ench, false, a.get(), 1.0f, false, 0.0f, pl);
                ++hit;
            }
            if (hit > 0) {
                spdlog::info("[echo] weapon AoE r={:.0f} → {} nearby enemy(ies)", radius, hit);
            }
        });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// m19: re-key socket records across container transfers (see
// RekeyTransferredSockets). Cheap gate: only defer a task when the moved
// base form has records at all.
class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
public:
    static ContainerSink* GetSingleton() {
        static ContainerSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                          RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (!a_event || !a_event->baseObj || !a_event->newContainer) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const RE::FormID base = a_event->baseObj;
        // m27: a gem landed in the player's bags (loot, purchase, unsocket
        // return) - tuck it into the pouch container.
        if (a_event->newContainer == 0x14 && g_gemByItem.contains(base)) {
            SKSE::GetTaskInterface()->AddTask([]() {
                EnsurePouchRef();
                RouteGemsToPouch();
                if (g_needSeedDiscoveries) { SeedDiscoveries(); g_needSeedDiscoveries = false; }
                CheckGemDiscoveries();  // m37: study newly-acquired gem families
                // m32h (marth: 'delayed sync between pouch and inv'): a gem
                // that just routed to the pouch got a new uid — an open menu's
                // snapshot now holds the stale one. Rebuild it so socketing
                // always sees the gem's current uid.
                if (g_menu.open.load()) {
                    BuildMenuSnapshot();
                }
            });
        }
        // m23: a convertible enchanted generic just landed somewhere (chest
        // generation, corpse loot, purchase, pickup) — convert it in place.
        if (g_convert.contains(base)) {
            const RE::FormID holder = a_event->newContainer;
            SKSE::GetTaskInterface()->AddTask([holder]() {
                if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(holder)) {
                    ConvertInventory(ref);
                }
            });
        }
        bool ours = false;
        for (const auto& [key, rec] : g_sockets) {
            if (static_cast<RE::FormID>(key >> 24) == base) {
                ours = true;
                break;
            }
        }
        if (!ours) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const RE::FormID    oldC = a_event->oldContainer;
        const RE::FormID    newC = a_event->newContainer;
        const std::uint16_t evUid = a_event->uniqueID;
        SKSE::GetTaskInterface()->AddTask([base, oldC, newC, evUid]() {
            RekeyTransferredSockets(base, oldC, newC, evUid);
        });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// m19e: outdoors, cell-attach fires before actors equip their gear (no worn
// candidates yet -> silent miss; marth saw 1 socketed orc in 20+ at 0.24).
// TESObjectLoadedEvent fires when the ref's 3D loads — equipment is worn by
// then. Both triggers stay live: the deterministic hash + already-blessed
// guard make MaybeStampNPCGear idempotent, so whichever fires second no-ops.
class ObjectLoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
public:
    static ObjectLoadedSink* GetSingleton() {
        static ObjectLoadedSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
                                          RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (!a_event || !a_event->loaded) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const RE::FormID id = a_event->formID;
        SKSE::GetTaskInterface()->AddTask([id]() {
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
                if (auto* actor = ref->As<RE::Actor>()) {
                    ConvertInventory(actor);
                    MaybeStampNPCGear(actor);
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
// m33: the grindstone/workbench gate enchanted tempering on Arcane Blacksmith
// (Skyrim.esm 0x05218E — Requiem overrides in place; no entry point, engine
// hardcodes it by FormID). Socketed items read as enchanted, so the only lever
// is the perk itself. bTemperNoPerk ON = ensure present (marking it MEO-added
// so OFF can revoke ONLY our grant, never a perk the player earned). Because
// conversion sockets all generic enchanted loot, this effectively frees only
// socketed gear (artifacts sit under a different perk).
void ApplyTemperPerk() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !g_perkArcaneBlacksmith) {
        return;
    }
    const bool has = player->HasPerk(g_perkArcaneBlacksmith);
    if (g_temperNoPerk && !has) {
        player->AddPerk(g_perkArcaneBlacksmith, 1);
        g_meoGrantedArcane = true;
        spdlog::info("[temper] granted Arcane Blacksmith — socketed gear improvable without it");
    } else if (!g_temperNoPerk && g_meoGrantedArcane && has) {
        player->RemovePerk(g_perkArcaneBlacksmith);
        g_meoGrantedArcane = false;
        spdlog::info("[temper] revoked MEO-granted Arcane Blacksmith (toggle off)");
    }
}

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
    ApplyTemperPerk();  // m33: socketed gear temperable w/o Arcane Blacksmith (MCM)
    // m36j: the normal-gem starter kit AND the support-gem test scaffold are both
    // gone for 1.0 — acquisition is found-loot-driven (level-15+ boss loot +
    // hand-placed, plus normal-gem drops/conversion). The g_starterGranted /
    // g_armorStarterGranted / g_supportScaffoldGranted co-save flags are still
    // read/written (schema stability) but grant nothing.
}

// On load, worn socketed gems' abilities are runtime state that doesn't persist
// — re-activate every worn socketed item so effects fire without a manual
// re-equip. (The g_sockets records + item ExtraEnchantment persist fine; the
// runtime magic delivery does not.)
//
// MECHANISM (m16): the m15 [load-diag] settled it — on load the enchant is
// FULLY intact (kCostOverride=true, costOverride=0, charge=0xFFFF all survive).
// So the regression was never the enchant data. The dead thing is the actor's
// equipped-weapon delivery cache, which the engine rebuilds from the item's
// BASE-form enchantment (none for us — ours is an instance ExtraEnchantment),
// not from ExtraEnchantment, on load. Actor::UpdateWeaponAbility does NOT
// rebuild it here: m15 called it 4x across 8s and the sword still never fired;
// only a manual unequip/re-equip did. So the fix is to drive the engine's own
// equip flow — a real unequip -> re-equip — which is exactly what the player
// did by hand. a_rebuild refreshes the created enchant (picks up MCM magnitude);
// a_reequip does the equip cycle. Targets are collected BEFORE re-equipping so
// the equip calls can't invalidate the entryList mid-iteration.
// m24b (marth's "improperly escalated skills"): before the m23c worn-teardown
// fix, unsocketing a worn item's LAST gem orphaned its constant ability — the
// ActiveEffect (with its fortify-skill / resist AV modifier) could ride along
// in the save with no owning socket. Sweep the player's active effects for
// OUR signature — engine-created FF enchantment with kCostOverride+cost 0
// (every MEO instance enchant; player-crafted enchants never set that) whose
// base effect is in the gem MGEF set — that no currently-worn socketed item
// vouches for, and dispel. Dispel runs the effect's recover path, so the AV
// modifier is returned. Runs after each post-load refresh pass; every kill
// is NAMED in the log ([avfix]) — that log answers "which skills were from
// bad gem socketing".
void DispelStaleGemEffects() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* mt = player ? player->AsMagicTarget() : nullptr;
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!mt || !changes || !changes->entryList) {
        return;
    }
    // m26e (marth's "removed a non gem effect"): protect enchants attached
    // to ANY inventory item, worn or not. The old worn-only set raced
    // EquipCycleWorn — mid-cycle the item is briefly unworn, its own live
    // enchant fell out of the set, and the sweep dispelled it; if the
    // deferred re-equip then deduped against stale bookkeeping (§8 rule),
    // the effect stayed MISSING. An orphan is an enchant attached to
    // NOTHING, not one attached to something momentarily unequipped.
    std::unordered_set<RE::FormID> valid;
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || !entry->extraLists) {
            continue;
        }
        for (auto* xl : *entry->extraLists) {
            if (!xl) {
                continue;
            }
            if (auto* xe = xl->GetByType<RE::ExtraEnchantment>(); xe && xe->enchantment) {
                valid.insert(xe->enchantment->GetFormID());
            }
        }
    }
    std::unordered_set<const RE::EffectSetting*> gemFx;
    for (const auto& rg : g_gems) {
        if (rg.mgef) {
            gemFx.insert(rg.mgef);
        }
        for (auto* m : rg.mgefLv) {
            if (m) {
                gemFx.insert(m);
            }
        }
        for (int r = 0; r < rg.nRiders; ++r) {
            if (rg.riders[r].mgef) {
                gemFx.insert(rg.riders[r].mgef);
            }
        }
    }
    auto* list = mt->GetActiveEffectList();
    if (!list) {
        return;
    }
    std::vector<RE::ActiveEffect*> stale;
    // m32e (marth's deck finding): the valid-guard shielded SAME-FORM
    // duplicates — every extra registration of a still-worn enchant shares
    // that worn form, so orphan logic never touched them while refresh
    // passes could stack them. Track (ench form, effect) pairs among OUR
    // effects and dispel every copy after the first, vouched or not.
    std::unordered_set<std::uint64_t> seen;
    for (auto* ae : *list) {
        if (!ae || !ae->spell) {
            continue;
        }
        auto* en = ae->spell->As<RE::EnchantmentItem>();
        if (!en || en->GetFormID() < 0xFF000000u) {
            continue;  // not an engine-created enchantment
        }
        if (!en->data.flags.any(RE::EnchantmentItem::EnchantmentFlag::kCostOverride) ||
            en->data.costOverride != 0) {
            continue;  // not MEO's free-enchant signature
        }
        const auto* mgef = ae->GetBaseObject();
        if (!mgef || !gemFx.contains(mgef)) {
            continue;
        }
        const std::uint64_t pair =
            (static_cast<std::uint64_t>(en->GetFormID()) << 32) | mgef->GetFormID();
        if (!seen.insert(pair).second) {
            spdlog::info("[avfix] duplicate '{}' from ench {:08X} — dispelling the extra",
                         mgef->GetName(), en->GetFormID());
            stale.push_back(ae);
            continue;
        }
        if (valid.contains(en->GetFormID())) {
            continue;  // a worn socketed item vouches for (one copy of) it
        }
        stale.push_back(ae);
    }
    for (auto* ae : stale) {
        const auto* mgef = ae->GetBaseObject();
        spdlog::info("[avfix] dispelling stale gem effect '{}' ({:08X}) mag={:.1f}",
                     mgef->GetName(), mgef->GetFormID(), ae->magnitude);
        ae->Dispel(true);
    }
    if (!stale.empty()) {
        spdlog::info("[avfix] {} stale gem effect(s) dispelled — modifiers recovered", stale.size());
        Notify(std::format("MEO: cleaned {} stale gem effect(s) from your save.", stale.size()));
    }
}

// m36n (marth): reactivate WORN socketed gear on current followers, so a slotted
// weapon/armor handed to a companion keeps firing across a save/load. The main
// reapply was player-only, leaving follower gear dormant until a manual re-equip.
// Same idempotent equip-cycle teardown the player path uses; runs on the deferred
// load passes alongside the player's.
void ReapplyFollowerSockets() {
    auto* lists = RE::ProcessLists::GetSingleton();
    if (!lists) {
        return;
    }
    int done = 0;
    for (auto& h : lists->highActorHandles) {
        auto a = h.get();
        if (!a || a->IsPlayerRef() || !a->IsPlayerTeammate() || a->IsDead()) {
            continue;
        }
        auto* changes = a->GetInventoryChanges();
        if (!changes || !changes->entryList) {
            continue;
        }
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object || !entry->extraLists ||
                !(entry->object->Is(RE::FormType::Weapon) ||
                  entry->object->Is(RE::FormType::Armor))) {
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
                if (ours) {
                    EquipCycleWorn(a.get(), entry->object, xl);
                    ++done;
                }
            }
        }
    }
    if (done > 0) {
        spdlog::info("[load] reactivated {} follower worn socket(s)", done);
    }
}

void ReapplyWornSockets(bool a_rebuild, bool a_reequip, bool a_diag = false) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }
    struct Target { RE::TESBoundObject* base; RE::ExtraDataList* xl; bool isArmor; bool left; };
    std::vector<Target> targets;
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
            if (a_diag) {
                // m19e forensics (marth's "loaded ungemmed" report): the full
                // truth for every worn piece — uid (or none), each slot's
                // record (or NONE), and whether an enchant extra is present.
                std::string slots;
                if (xid) {
                    for (int s = 0; s < kMaxSockets; ++s) {
                        auto it = g_sockets.find(MakeKey(entry->object->GetFormID(),
                                                         xid->uniqueID,
                                                         static_cast<std::uint8_t>(s)));
                        if (it != g_sockets.end()) {
                            slots += std::format(" s{}={}L{}", s, it->second.gid,
                                                 it->second.level);
                        }
                    }
                }
                // m35d: the double-source smoking gun. A socketed item's base
                // form must carry NO formEnchanting (IsSocketable* requires it),
                // so the ONLY enchant source is the MEO ExtraEnchantment. If a
                // base enchant is present here, the engine applies it AND the
                // gem — the effect shows "as base item and renamed item".
                RE::EnchantmentItem* baseEnch = nullptr;
                if (auto* w = entry->object->As<RE::TESObjectWEAP>()) {
                    baseEnch = w->formEnchanting;
                } else if (auto* ar = entry->object->As<RE::TESObjectARMO>()) {
                    baseEnch = ar->formEnchanting;
                }
                spdlog::info("[load-diag] worn {:08X} '{}' uid={}{} ench={} baseEnch={:08X}",
                             entry->object->GetFormID(), entry->object->GetName(),
                             xid ? std::to_string(xid->uniqueID) : "none",
                             slots.empty() ? " records=NONE" : slots,
                             xl->HasType(RE::ExtraDataType::kEnchantment),
                             baseEnch ? baseEnch->GetFormID() : 0u);
            }
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
            if (a_diag) {  // read the as-loaded enchant before we touch it
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
            targets.push_back({ entry->object, xl, entry->object->Is(RE::FormType::Armor),
                                xl->HasType(RE::ExtraDataType::kWornLeft) });
        }
    }
    if (a_reequip) {
        spdlog::info("[load] refresh pass: paused={} player3D={} drawn={}",
                     RE::UI::GetSingleton() && RE::UI::GetSingleton()->GameIsPaused(),
                     player->Is3DLoaded(),
                     player->AsActorState() && player->AsActorState()->IsWeaponDrawn());
        // m35d (marth's "effect stacks as base item + renamed item"): the m19f
        // blinkless strip/restamp is retired. It relied on Update*Ability to
        // REPLACE the worn ability, but that call early-outs on teardown (when
        // the enchant extra is gone it never unregisters — m23c) and the
        // restamp minted a NEW FF form every pass, so the old ability lingered
        // beside the new one. avfix could only clean the copies it recognized;
        // a cross-form pair that both still looked attached survived, and the
        // player saw the effect twice. The engine's OWN complete teardown is
        // the equip cycle (m16/m17b/m23c proven, used by every in-session
        // socket path) and it is IDEMPOTENT — unequip fully drops the old
        // ability, equip installs exactly ONE from the current ExtraEnchantment,
        // so running it any number of passes can never accumulate. Its
        // one-frame gear blink lands behind the post-load fade — a small price
        // for a reactivation that cannot duplicate. (The m19f "no blink" win
        // was elegance, not correctness; correctness wins here.)
        for (auto& t : targets) {
            EquipCycleWorn(player, t.base, t.xl);  // full teardown → one ability
        }
        DispelStaleGemEffects();  // m24b backstop: clear any save-carried orphan
        ReapplyFollowerSockets();  // m36n: companions' slotted gear too
        spdlog::info("[load] refresh pass: {} worn socket(s) re-activated (equip cycle)",
                     static_cast<int>(targets.size()));
    }
    // Mechanism probe (ENGINE_NOTES §8 epistemic status): the hit path may gate
    // on the actor's item-charge AVs. Log them as-loaded (diag pass) and after
    // the re-equip — one load with a socketed weapon settles the theory.
    if ((a_diag || a_reequip) && !targets.empty()) {
        if (auto* avo = player->AsActorValueOwner()) {
            spdlog::info("[load-diag] itemCharge AVs {} re-equip: L={:.0f} R={:.0f}",
                         a_reequip ? "AFTER" : "BEFORE",
                         avo->GetActorValue(RE::ActorValue::kLeftItemCharge),
                         avo->GetActorValue(RE::ActorValue::kRightItemCharge));
        }
    }
    spdlog::info("[load] {} worn socketed item(s) (rebuild={}, reequip={})",
                 static_cast<int>(targets.size()), a_rebuild, a_reequip);
}

// On load: refresh the enchant immediately (+ diag), then drive ONE real
// unequip/re-equip once the game is actually LIVE. m16 used a blind +4s
// timer; on a heavy-area load (Solitude, 2026-07-09) it fired while the
// loading screen was still up and the equip cycle was swallowed — worn gems
// stayed FX/delivery-dead until a manual socket action. The reliable anchor
// is the Loading Menu CLOSING (gameplay resumed): MenuSink consumes
// g_pendingReapply on that close (+1.5s of fade margin). A 15s fallback
// timer covers loads that never show a loading menu.
std::atomic<bool> g_pendingReapply{ false };

void RunDeferredReapply(int a_delayMs) {
    std::thread([a_delayMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(a_delayMs));
        SKSE::GetTaskInterface()->AddTask([]() {
            ReapplyWornSockets(/*rebuild=*/false, /*reequip=*/true);
        });
    }).detach();
}

void ScheduleReapplyWornSockets() {
    SKSE::GetTaskInterface()->AddTask([]() {
        ReapplyWornSockets(/*rebuild=*/true, /*reequip=*/false, /*diag=*/true);
    });
    g_pendingReapply = true;
    std::thread([]() {  // fallback: if no Loading Menu close consumes the flag
        std::this_thread::sleep_for(std::chrono::milliseconds(15000));
        if (g_pendingReapply.exchange(false)) {
            SKSE::GetTaskInterface()->AddTask([]() {
                ReapplyWornSockets(/*rebuild=*/false, /*reequip=*/true);
            });
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
    a_intfc->WriteRecordData(g_pouchRefID);  // v7 field: hidden gem container ref
    const std::uint8_t grantedArcane = g_meoGrantedArcane ? 1 : 0;  // v8 field
    a_intfc->WriteRecordData(grantedArcane);
    const std::uint8_t supportScaffold = g_supportScaffoldGranted ? 1 : 0;  // v9 field
    a_intfc->WriteRecordData(supportScaffold);
    a_intfc->WriteRecordData(g_handPlacedMask);  // v10 field: hand-placed support gems dropped
    const std::uint32_t discN = static_cast<std::uint32_t>(g_discoveredGems.size());  // v11
    a_intfc->WriteRecordData(discN);
    for (const auto& gid : g_discoveredGems) {
        const std::uint16_t len = static_cast<std::uint16_t>(gid.size());
        a_intfc->WriteRecordData(len);
        a_intfc->WriteRecordData(gid.data(), len);
    }
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
    g_pouchRefID = 0;
    g_meoGrantedArcane = false;
    g_nextUID = 0x9000;
    g_starterGranted = false;
    g_mentorGranted = false;
    g_armorStarterGranted = false;
    g_supportScaffoldGranted = false;
    g_handPlacedMask = 0;
    g_discoveredGems.clear();
    g_needSeedDiscoveries = false;
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
        if (version >= 7) {  // v7: pouch container ref (recreated when absent)
            a_intfc->ReadRecordData(g_pouchRefID);
        }
        if (version >= 8) {  // v8: MEO-granted Arcane Blacksmith flag
            std::uint8_t grantedArcane = 0;
            a_intfc->ReadRecordData(grantedArcane);
            g_meoGrantedArcane = grantedArcane != 0;
        }
        if (version >= 9) {  // v9: support test-scaffold handed out (older saves = false)
            std::uint8_t supportScaffold = 0;
            a_intfc->ReadRecordData(supportScaffold);
            g_supportScaffoldGranted = supportScaffold != 0;
        }
        if (version >= 10) {  // v10: hand-placed support-gem bitmask (older saves = 0)
            a_intfc->ReadRecordData(g_handPlacedMask);
        }
        if (version >= 11) {  // v11: discovered gem families
            std::uint32_t discN = 0;
            a_intfc->ReadRecordData(discN);
            for (std::uint32_t i = 0; i < discN; ++i) {
                std::uint16_t len = 0;
                a_intfc->ReadRecordData(len);
                std::string gid(len, '\0');
                a_intfc->ReadRecordData(gid.data(), len);
                g_discoveredGems.insert(std::move(gid));
            }
        } else {
            // Pre-v11 save: seed already-held/socketed gems as discovered on load
            // (silently, no XP burst) so only genuinely-new finds grant afterward.
            g_needSeedDiscoveries = true;
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
    for (const auto& [key, rec] : g_sockets) {  // m19e forensics
        spdlog::info("[load]   rec base={:08X} uid={} slot={} gid={} L{} xp={:.0f}",
                     static_cast<RE::FormID>(key >> 24),
                     static_cast<std::uint16_t>((key >> 8) & 0xFFFF),
                     static_cast<int>(key & 0xFF), rec.gid, rec.level, rec.xp);
    }
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_pouchRefID = 0;  // m27: pouch ref is save-scoped
    g_meoGrantedArcane = false;  // m33
    g_sockets.clear();
    g_nextUID = 0x9000;
    g_starterGranted = false;
    g_mentorGranted = false;
    g_armorStarterGranted = false;
    g_supportScaffoldGranted = false;
    g_handPlacedMask = 0;
    g_discoveredGems.clear();
    g_needSeedDiscoveries = false;
    CloseGemMenu();
    spdlog::info("[revert] socket index cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        LoadCalibration();
        ResolveCatalog();
        ReadConfig();
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());  // m36 Echo AoE
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellAttachDetachEvent>(CellAttachSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESObjectLoadedEvent>(ObjectLoadedSink::GetSingleton());
        SKSE::GetCrosshairRefEventSource()->AddEventSink(CrosshairSink::GetSingleton());
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        StartEchoHeartbeat();  // m36: Echo armor follower-share
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v1.0.2 loaded");
        }
        spdlog::info("kDataLoaded: MEO M6 live; SpellCast + Death + CellAttach + CrosshairRef sinks + render/input hooks");
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        // After LoadCallback/Revert — co-save flags are current here.
        SKSE::GetTaskInterface()->AddTask([]() {
            EnsurePlayerSetup();
            RefreshPerks();
            // m23: convert enchanted generics the player already owns
            // (mid-save installs; new acquisitions convert via ContainerSink).
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                ConvertInventory(player);
            }
            EnsurePouchRef();     // m27: gems live in the hidden pouch container
            RouteGemsToPouch();
            if (g_needSeedDiscoveries) { SeedDiscoveries(); g_needSeedDiscoveries = false; }
            CheckGemDiscoveries();  // m37: study newly-acquired gem families
            TryPlaceHandPlaced();  // m36i: place persistent hand-placed gems now (Offering Box);
                                   // temporary boss-chest refs place on cell-attach
            // m32d: recovery runs EVERY load — the creation-only gate missed
            // saves holding an alive-but-looted pouch (saved between purge
            // cycles). A healthy load strands nothing and this no-ops; the
            // only false positive is a gem stored in a WORLD chest, which
            // the log would name loudly.
            g_pouchCreatedThisLoad = false;
            RecoverStrandedGems();
            if (g_purgeSupportGems) {  // m36l: dev cleanup, after recovery so nothing lingers
                PurgeLooseSupportGems();
            }
            if (auto* pouch = PouchRef()) {  // status is never ambiguous again
                int inst = 0, plain = 0;
                for (const auto& [obj, data] : pouch->GetInventory(
                         [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc); })) {
                    if (data.first <= 0 || !g_gemByItem.contains(obj->GetFormID())) {
                        continue;
                    }
                    int withUid = 0;
                    if (data.second && data.second->extraLists) {
                        for (auto* xl : *data.second->extraLists) {
                            withUid += xl && xl->GetByType<RE::ExtraUniqueID>() ? 1 : 0;
                        }
                    }
                    inst += withUid;
                    plain += data.first - withUid;
                }
                spdlog::info("[pouch] ref {:08X} alive: {} gem instance(s), {} plain",
                             g_pouchRefID, inst, plain);
            } else {
                spdlog::warn("[pouch] NO POUCH after ensure — gems would stay in inventory");
            }
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
    spdlog::info("MEO native v1.0.2 loading; runtime {}", gameVersion.string());
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
