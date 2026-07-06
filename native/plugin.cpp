// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M2a self-describing display (Docs/NATIVE_REWRITE_PLAN.md, DESIGN §2): proves
// the core loot requirement — a socketed item shows the SAME name in the world
// (dropped, chest, boss corpse) as in inventory, with NO transform on pickup —
// via the modern engine-native path: attach an ExtraTextDisplayData to the item
// INSTANCE. That extra data is engine-serialized and travels with the instance
// everywhere it renders (exactly how a vanilla "Iron Sword of Sparks" shows its
// name on the floor). Papyrus can't do this off the worn slot; C++ can, on any
// instance — native-plan reason #2.
//
// Trigger for the M2a test: a TESEquipEvent sink (standard SKSE pattern, not a
// code hook). On equipping a weapon, the DLL stamps "[MEO] <name>" onto the worn
// instance once. Test: equip a dagger -> renamed -> DROP it -> the ground item
// shows "[MEO] Glass Dagger" -> pick it up -> still "[MEO] Glass Dagger", no
// flicker. The event's uniqueID is logged as recon for M2b's per-instance
// identity (co-save leveling record + ExtraEnchantment).
//
// The M1 co-save serialization stays armed underneath (proven persistence
// substrate); M2b populates it with real records.

#include <spdlog/sinks/basic_file_sink.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace {

// ── Per-instance socket index (M1 substrate; populated in M2b) ────────
struct GemRecord {
    std::uint32_t gemType;
    std::uint32_t level;
    float         xp;
};
std::unordered_map<std::uint32_t, GemRecord> g_gems;

constexpr std::uint32_t kSerID = 'MEO1';
constexpr std::uint32_t kRecGems = 'GEMS';
constexpr std::uint32_t kSerVersion = 1;

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

// ── M2a: stamp a persistent display name onto a worn item instance ────
// Runs on the main thread (deferred via the task queue) after the equip
// settles, so we never mutate an extra list mid-event.
void RenameWornInstance(RE::FormID a_baseID) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* changes = player ? player->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList) {
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
            if (auto* existing = xList->GetByType<RE::ExtraTextDisplayData>()) {
                spdlog::info("worn {:08X} already named '{}' — leaving it",
                             a_baseID, existing->displayName.c_str());
                return;
            }
            const char* baseName = entry->object->GetName();
            const std::string newName = std::string("[MEO] ") + (baseName && *baseName ? baseName : "Item");
            xList->Add(new RE::ExtraTextDisplayData(newName.c_str()));
            spdlog::info("RENAMED worn instance {:08X} -> '{}' (shows everywhere: inventory, ground, chest, corpse)",
                         a_baseID, newName);
            return;
        }
    }
    spdlog::warn("RenameWornInstance: no worn instance of {:08X} found", a_baseID);
}

// ── TESEquipEvent sink (test trigger for M2a) ────────────────────────
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

        // Recon for M2b: does the engine hand us a per-instance uniqueID?
        spdlog::info("equip weapon {:08X} '{}' uniqueID={} (0 = untracked/stacked)",
                     a_event->baseObject, base->GetName(), a_event->uniqueID);

        const RE::FormID baseID = a_event->baseObject;
        SKSE::GetTaskInterface()->AddTask([baseID]() { RenameWornInstance(baseID); });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── SKSE co-save callbacks (M1 substrate, armed; empty until M2b) ─────
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    if (!a_intfc->OpenRecord(kRecGems, kSerVersion)) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(g_gems.size());
    a_intfc->WriteRecordData(count);
    for (const auto& [id, rec] : g_gems) {
        a_intfc->WriteRecordData(id);
        a_intfc->WriteRecordData(rec);
    }
    spdlog::info("[save] gem index: {} record(s)", g_gems.size());
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_gems.clear();
    std::uint32_t type = 0, version = 0, length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kRecGems || version != kSerVersion) {
            continue;
        }
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t id = 0;
            GemRecord rec{};
            a_intfc->ReadRecordData(id);
            a_intfc->ReadRecordData(rec);
            g_gems[id] = rec;
        }
    }
    spdlog::info("[load] gem index: {} record(s)", g_gems.size());
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_gems.clear();
    spdlog::info("[revert] gem index cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.4.0 (M2a display) loaded — equip a weapon to rename its instance");
        }
        spdlog::info("kDataLoaded: MEO M2a live; TESEquipEvent sink registered (no code hooks)");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.4.0 (M2a self-describing display) loading; runtime {}", gameVersion.string());
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
