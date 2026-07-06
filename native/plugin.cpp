// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M2b real socket state (Docs/NATIVE_REWRITE_PLAN.md, DESIGN §2): the worn
// instance now gets the FULL self-describing bundle —
//   ExtraUniqueID        (distinct instance; co-save key; no stacking)
//   ExtraTextDisplayData (display name, engine-rendered everywhere)
//   ExtraEnchantment     (a REAL enchantment: EnchWeaponFireDamage01, verified
//                         0x00049BB7 in Skyrim.esm — fire damage on hit)
// plus a co-save record uid -> {gemType, level, xp} (the M1-proven substrate,
// now carrying real data; the uid counter is persisted too, replacing M2a.1's
// session-only stub).
//
// Why ExtraEnchantment matters beyond the effect: M2a.1 showed a bare rename is
// visible on the ground (crosshair) but the vanilla pickup prompt still reads a
// different name source. P0's Papyrus rename displayed fine everywhere — and
// that item carried a real runtime enchantment. An enchanted instance goes
// through the engine's enchanted-item display paths; this build tests exactly
// that (Marth's call: fold in ExtraEnchantment next).
//
// Test: equip a weapon -> it becomes "[MEO] <name>", carries a real fire
// enchant (hits burn, charge bar appears), doesn't stack; drop it -> ground
// name AND pickup prompt should now agree; save/reload -> log shows the uid
// record restored with the same uid. Still zero code hooks (event sink + task
// queue + serialization only).

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

// EnchWeaponFireDamage01 — verified against Skyrim.esm by tools/parse_ench.py.
constexpr RE::FormID kFireEnchID = 0x00049BB7;

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
    auto* ench = RE::TESForm::LookupByID<RE::EnchantmentItem>(kFireEnchID);
    if (!ench) {
        spdlog::error("EnchWeaponFireDamage01 ({:08X}) not found — no socket applied", kFireEnchID);
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

            if (!xList->GetByType<RE::ExtraTextDisplayData>()) {
                const char* baseName = entry->object->GetName();
                const std::string newName =
                    std::string("[MEO] ") + (baseName && *baseName ? baseName : "Item");
                xList->Add(new RE::ExtraTextDisplayData(newName.c_str()));
            }

            xList->Add(new RE::ExtraEnchantment(ench, 500, false));

            g_gems[uid] = GemRecord{ 1u, 1u, 0.0f };  // Fire gem, level 1
            spdlog::info("SOCKETED worn {:08X}: uid={} ench='{}' ({:08X}) charge=500; index now {} record(s)",
                         a_baseID, uid, ench->GetName(), kFireEnchID, g_gems.size());
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
            console->Print("MEO native v0.5.0 (M2b real socket) loaded — equip an unenchanted weapon");
        }
        spdlog::info("kDataLoaded: MEO M2b live; TESEquipEvent sink registered (no code hooks)");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.5.0 (M2b real socket state) loading; runtime {}", gameVersion.string());
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
