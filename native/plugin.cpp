// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M1 co-save index (Docs/NATIVE_REWRITE_PLAN.md): the load-bearing persistence
// proof for the per-instance design (Marth: "the design falls apart when every
// gem isn't tracked uniquely"). We own an in-memory map of unique instance ID ->
// {gem type, level, XP} and serialize it through the SKSE co-save Save/Load/
// Revert callbacks — engine-native storage, no Papyrus VM data, no save bloat.
//
// This build has ZERO engine hooks; serialization callbacks are SKSE-sanctioned
// interfaces, not code hooks. To prove the round-trip with no ESP/Papyrus (same
// install-and-read-the-log test as M0), it AUTO-SEEDS one synthetic record on
// every save and logs the whole map on save/load/revert. Test: save -> reload
// and the log shows the accumulated records restored; New Game reverts to empty.
// M2 replaces the synthetic seeding with real item instance IDs + applied
// enchantments; the serialization substrate proven here stays.
//
// Structure adapted from MRO's CI-proven native/ and the CommonLibSSE-NG
// serialization pattern (po3 mods use this exact shape). Doctrine: copy a
// working thing, don't invent.

#include <spdlog/sinks/basic_file_sink.h>

#include <cstdint>
#include <unordered_map>

namespace {

// ── Per-instance socket index (the M1 subject) ───────────────────────
struct GemRecord {
    std::uint32_t gemType;  // signature-derived gem id (synthetic in M1)
    std::uint32_t level;    // 1..5
    float         xp;       // accumulated AP toward next level
};

std::unordered_map<std::uint32_t, GemRecord> g_gems;  // instanceID -> record
std::uint32_t g_nextInstanceId = 0;                   // monotonic id source

// Co-save identifiers. kSerID tags MEO's whole co-save section; kRecGems tags
// the gem-map record within it. Bump kSerVersion if the on-disk shape changes.
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

void LogMap(const char* when) {
    spdlog::info("[{}] gem index: {} record(s), nextInstanceId={}", when, g_gems.size(), g_nextInstanceId);
    for (const auto& [id, rec] : g_gems) {
        spdlog::info("    instance {} -> type={} level={} xp={:.1f}", id, rec.gemType, rec.level, rec.xp);
    }
}

// ── SKSE co-save callbacks ───────────────────────────────────────────
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    // M1 self-test: grow the index by one synthetic record so successive saves
    // accumulate distinct instances (proves uniqueness + growth persist). M2
    // removes this — real records are added when the player sockets a gem.
    const std::uint32_t id = ++g_nextInstanceId;
    g_gems[id] = GemRecord{ id * 10u, 1u, 0.0f };
    spdlog::info("SaveCallback: seeded synthetic instance {} (M1 self-test)", id);

    if (!a_intfc->OpenRecord(kRecGems, kSerVersion)) {
        spdlog::error("SaveCallback: OpenRecord('GEMS') failed");
        return;
    }
    a_intfc->WriteRecordData(g_nextInstanceId);
    const std::uint32_t count = static_cast<std::uint32_t>(g_gems.size());
    a_intfc->WriteRecordData(count);
    for (const auto& [instId, rec] : g_gems) {
        a_intfc->WriteRecordData(instId);
        a_intfc->WriteRecordData(rec);
    }
    LogMap("save");
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_gems.clear();
    g_nextInstanceId = 0;

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kRecGems) {
            spdlog::warn("LoadCallback: skipping unknown record '{:08X}'", type);
            continue;
        }
        if (version != kSerVersion) {
            spdlog::warn("LoadCallback: 'GEMS' version {} != {} — skipping", version, kSerVersion);
            continue;
        }
        a_intfc->ReadRecordData(g_nextInstanceId);
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t instId = 0;
            GemRecord rec{};
            a_intfc->ReadRecordData(instId);
            a_intfc->ReadRecordData(rec);
            g_gems[instId] = rec;
        }
    }
    LogMap("load");
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_gems.clear();
    g_nextInstanceId = 0;
    LogMap("revert");
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.3.0 (M1 co-save index) loaded — no hooks active");
        }
        spdlog::info("kDataLoaded: MEO native M1 is live (co-save serialization armed, no hooks)");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.3.0 (M1 co-save index) loading; runtime {}", gameVersion.string());
    if (gameVersion != REL::Version(1, 6, 1170, 0)) {
        spdlog::warn("Untested runtime {} (built against 1.6.1170)", gameVersion.string());
    }

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(kSerID);
    serialization->SetSaveCallback(SaveCallback);
    serialization->SetLoadCallback(LoadCallback);
    serialization->SetRevertCallback(RevertCallback);
    spdlog::info("Serialization registered (id 'MEO1'); Save/Load/Revert armed");

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    spdlog::info("SKSEPluginLoad complete; messaging listener registered");
    return true;
}
