// MEO native plugin — MEO.dll (CommonLibSSE-NG).
//
// M0 skeleton (Docs/NATIVE_REWRITE_PLAN.md): proves the whole build+load
// pipeline before ANY hook exists. The DLL loads, resolves the SKSE log
// directory, and logs its version and the running game version to MEO.log,
// then prints a one-line banner to the console on data load. That's it —
// zero hooks, zero engine reads, nothing that can CTD. Every later milestone
// (co-save index, native socket apply, XP/proc hooks) lands one at a time on
// top of this, each user-tested in game before the next.
//
// Structure adapted from MRO's working native/ (this repo's sibling), which
// is CI-built against CommonLibSSE-NG. MRO's *systems* (vendor gold, DR) share
// nothing with MEO and are intentionally NOT ported here — only the toolchain
// and the load/log scaffold.

#include <spdlog/sinks/basic_file_sink.h>

namespace {

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

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MEO native v0.2.0 (M0 skeleton) loaded — no hooks active");
        }
        spdlog::info("kDataLoaded: MEO native M0 skeleton is live (no hooks)");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MEO native v0.2.0 (M0 skeleton) loading; runtime {}", gameVersion.string());
    if (gameVersion != REL::Version(1, 6, 1170, 0)) {
        spdlog::warn("Untested runtime {} (built against 1.6.1170)", gameVersion.string());
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    spdlog::info("SKSEPluginLoad complete; messaging listener registered");
    return true;
}
