// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M2d: force the display name. M2c PROVED the enchant layer in-game (created
// enchant works, survives drop/pickup/save/reload) but the rename never
// showed: the engine lazily creates a blank ExtraTextDisplayData for tempered
// items (ExtraDataList::GetDisplayName when health != 1.0), so add-if-absent
// skipped every tempered weapon. M2d reuses/forces the record instead
// (SKSE SetDisplayName force=true semantics) and logs what it found.
//
// M2c: replicate P0's PROVEN enchant recipe natively. M2b (attach the base
// ENCH EnchWeaponFireDamage01 via ExtraEnchantment) showed the enchantment
// DESCRIPTION on the item card but applied no actual effect and no rename.
// SKSE's own source (PapyrusWornObject.cpp — the native code behind P0's
// WornObject.CreateEnchantment, which worked in ALL UIs with real effects)
// does two things M2b didn't:
//   1. It never attaches a base ENCH — it builds a player-style CREATED
//      enchantment via PersistentFormManager::CreateOffensiveEnchantment
//      (= RE::BGSCreatedObjectManager::AddWeaponEnchantment in NG) from
//      MGEF effect items.
//   2. After attaching, it calls Actor::UpdateWeaponAbility(base, xList,
//      leftHand) — THE call that activates the enchant's magic caster on the
//      already-equipped actor. Without it the UI reads ExtraEnchantment (so
//      the description shows) but no effect is ever attached — exactly the
//      M2b symptom.
// The bundle per instance is unchanged otherwise:
//   ExtraUniqueID        (distinct instance; co-save key; no stacking)
//   ExtraTextDisplayData (display name, engine-rendered everywhere)
//   ExtraEnchantment     (now a CREATED enchant: EnchFireDamageFFContact
//                         0x0004605A, verified in Skyrim.esm — the same MGEF
//                         the vanilla fire ENCH carries)
// plus the co-save record uid -> {gemType, level, xp}.
//
// Test: equip a NEVER-TOUCHED unenchanted weapon -> "[MEO] <name>", hits burn,
// charge bar; drop -> crosshair AND pickup prompt agree; save/reload -> same
// uid restored. Still zero code hooks (event sink + task queue + serialization
// only). NOTE: instances socketed by the dead M2b build still carry its inert
// ExtraEnchantment and are deliberately skipped — use a fresh weapon.

#include <spdlog/sinks/basic_file_sink.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace {

// ── Per-instance socket index (co-save) ──────────────────────────────
struct GemRecord {
    std::uint32_t gemType;  // 1 = Fire (M2b hard-wires the donor; catalog in M3)
    std::uint32_t level;    // 1..5
    float         xp;
};
std::unordered_map<std::uint16_t, GemRecord> g_gems;  // uid -> record
std::uint16_t g_nextUID = 0x9000;  // persisted; high range, clear of engine-assigned ids

constexpr std::uint32_t kSerID = 'MEO1';
constexpr std::uint32_t kRecGems = 'GEMS';
constexpr std::uint32_t kSerVersion = 2;  // v2: uid-keyed records + persisted counter

// EnchFireDamageFFContact — the MGEF inside EnchWeaponFireDamage01 (mag 5.0,
// contact delivery). Verified against Skyrim.esm by tools/parse_ench.py.
constexpr RE::FormID kFireMGEF = 0x0004605A;

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

// ── M2b: stamp the full socket bundle onto the worn instance ─────────
void SocketWornInstance(RE::FormID a_baseID) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
        return;
    }
    auto* mgef = RE::TESForm::LookupByID<RE::EffectSetting>(kFireMGEF);
    if (!mgef) {
        spdlog::error("EnchFireDamageFFContact ({:08X}) not found — no socket applied", kFireMGEF);
        return;
    }

    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object || entry->object->GetFormID() != a_baseID || !entry->extraLists) {
            continue;
        }
        for (auto* xList : *entry->extraLists) {
            if (!xList) {
                continue;
            }
            const bool worn = xList->HasType(RE::ExtraDataType::kWorn) ||
                              xList->HasType(RE::ExtraDataType::kWornLeft);
            if (!worn) {
                continue;
            }
            // Skip items that already carry a base or instance enchantment:
            // don't stomp vanilla enchanted gear in this test build.
            if (xList->HasType(RE::ExtraDataType::kEnchantment)) {
                spdlog::info("worn {:08X} already has an instance enchantment — leaving it", a_baseID);
                return;
            }
            if (auto* weap = entry->object->As<RE::TESObjectWEAP>(); weap && weap->formEnchanting) {
                spdlog::info("worn {:08X} has a BASE enchantment — leaving it", a_baseID);
                return;
            }

            std::uint16_t uid = 0;
            if (auto* xid = xList->GetByType<RE::ExtraUniqueID>()) {
                uid = xid->uniqueID;  // already ours from a previous session/equip
            } else {
                uid = g_nextUID++;
                xList->Add(new RE::ExtraUniqueID(a_baseID, uid));
            }

            // M2d: FORCE the display name. The engine lazily creates a blank
            // ExtraTextDisplayData for any TEMPERED item (ExtraDataList::
            // GetDisplayName, health != 1.0) and generates "Fine <name>" from
            // it — so the old only-add-if-absent logic silently skipped every
            // tempered weapon (the M2b/M2c "no rename"). Mirror SKSE's
            // SetDisplayName(force=true): reuse the existing record, clear any
            // message/quest owner, then SetName (which marks it kCustomName;
            // customNameLength keeps temper suffixes appendable).
            const char*       baseName = entry->object->GetName();
            const std::string newName =
                std::string("[MEO] ") + (baseName && *baseName ? baseName : "Item");
            if (auto* xText = xList->GetByType<RE::ExtraTextDisplayData>()) {
                spdlog::info("existing name data on {:08X}: '{}' ownerInstance={} temper={:.3f} — forcing MEO name",
                             a_baseID, xText->displayName.c_str(),
                             xText->ownerInstance.underlying(), xText->temperFactor);
                xText->displayNameText = nullptr;
                xText->ownerQuest = nullptr;
                xText->SetName(newName.c_str());
            } else {
                xList->Add(new RE::ExtraTextDisplayData(newName.c_str()));
            }

            // P0's proven recipe, step 1: a CREATED weapon enchantment from the
            // MGEF (what WornObject.CreateEnchantment does via
            // PersistentFormManager::CreateOffensiveEnchantment).
            RE::BSTArray<RE::Effect> effects;
            effects.resize(1);
            auto& eff = effects[0];
            eff.effectItem.magnitude = 10.0f;
            eff.effectItem.area = 0;
            eff.effectItem.duration = 0;
            eff.baseEffect = mgef;
            eff.cost = 0.0f;
            auto* ench = RE::BGSCreatedObjectManager::GetSingleton()->AddWeaponEnchantment(effects);
            if (!ench) {
                spdlog::error("AddWeaponEnchantment returned null — no socket applied");
                return;
            }
            xList->Add(new RE::ExtraEnchantment(ench, 500, false));

            g_gems[uid] = GemRecord{ 1u, 1u, 0.0f };  // Fire gem, level 1

            // P0's proven recipe, step 2: activate the enchant's magic caster on
            // the already-equipped actor (SKSE calls this after every
            // Set/CreateEnchantment; skipping it = description with no effect).
            const bool leftHand = xList->HasType(RE::ExtraDataType::kWornLeft);
            player->UpdateWeaponAbility(entry->object, xList, leftHand);

            spdlog::info("SOCKETED worn {:08X}: uid={} created ench {:08X} (MGEF {:08X} mag=10) charge=500, ability updated (left={}); index now {} record(s)",
                         a_baseID, uid, ench->GetFormID(), kFireMGEF, leftHand, g_gems.size());
            return;
        }
    }
    spdlog::warn("SocketWornInstance: no worn instance of {:08X} found", a_baseID);
}

// ── TESEquipEvent sink (test trigger) ────────────────────────────────
class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent> {
public:
    static EquipSink* GetSingleton() {
        static EquipSink singleton;
        return &singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
                                          RE::BSTEventSource<RE::TESEquipEvent>*) override {
        if (!a_event || !a_event->equipped) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* actor = a_event->actor.get();
        if (!actor || !actor->IsPlayerRef()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* base = RE::TESForm::LookupByID(a_event->baseObject);
        if (!base || !base->Is(RE::FormType::Weapon)) {
            return RE::BSEventNotifyControl::kContinue;
        }
        spdlog::info("equip weapon {:08X} '{}' uniqueID={}",
                     a_event->baseObject, base->GetName(), a_event->uniqueID);
        const RE::FormID baseID = a_event->baseObject;
        SKSE::GetTaskInterface()->AddTask([baseID]() { SocketWornInstance(baseID); });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── SKSE co-save callbacks ───────────────────────────────────────────
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    if (!a_intfc->OpenRecord(kRecGems, kSerVersion)) {
        spdlog::error("SaveCallback: OpenRecord('GEMS') failed");
        return;
    }
    a_intfc->WriteRecordData(g_nextUID);
    const std::uint32_t count = static_cast<std::uint32_t>(g_gems.size());
    a_intfc->WriteRecordData(count);
    for (const auto& [uid, rec] : g_gems) {
        a_intfc->WriteRecordData(uid);
        a_intfc->WriteRecordData(rec);
    }
    spdlog::info("[save] gem index: {} record(s), nextUID=0x{:X}", g_gems.size(), g_nextUID);
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_gems.clear();
    g_nextUID = 0x9000;
    std::uint32_t type = 0, version = 0, length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kRecGems) {
            continue;
        }
        if (version != kSerVersion) {
            spdlog::warn("[load] 'GEMS' version {} != {} — discarding (pre-M2b test data)", version, kSerVersion);
            continue;
        }
        a_intfc->ReadRecordData(g_nextUID);
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint16_t uid = 0;
            GemRecord rec{};
            a_intfc->ReadRecordData(uid);
            a_intfc->ReadRecordData(rec);
            g_gems[uid] = rec;
        }
    }
    spdlog::info("[load] gem index: {} record(s), nextUID=0x{:X}", g_gems.size(), g_nextUID);
    for (const auto& [uid, rec] : g_gems) {
        spdlog::info("    uid={} -> gemType={} level={} xp={:.1f}", uid, rec.gemType, rec.level, rec.xp);
    }
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_gems.clear();
    g_nextUID = 0x9000;
    spdlog::info("[revert] gem index cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.6.1 (M2d forced name) loaded — equip a fresh unenchanted weapon");
        }
        spdlog::info("kDataLoaded: MEO M2d live; TESEquipEvent sink registered (no code hooks)");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.6.1 (M2d forced display name) loading; runtime {}", gameVersion.string());
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
