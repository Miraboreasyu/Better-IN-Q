#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "betterinq_api.h"
#include "config.h"
#include "inq.h"

struct HINSTANCE__;
using HMODULE = HINSTANCE__*;

namespace api {

enum class CommandKind {
    PatchSessionConfig,
    ResetSessionOverrides,
    PersistConfig,
    RequestRestart,
    RequestResetOnly,
    RequestApplyActiveSet,
    RequestApplyLoadout,
    RequestQueueSet,
    RequestPouchRefill,
    CancelPending,
};

struct PendingCommand {
    std::uint64_t           requestId = 0;
    BetterINQ_Session       session = 0;
    CommandKind             kind = CommandKind::CancelPending;
    BetterINQ_ConfigPatch   patch{};
    BetterINQ_RestartParams restart{};
    BetterINQ_LoadoutParams loadout{};
    bool                    hasRestartParams = false;
    std::int32_t            setId = 0;
};

struct ExternalSessionOverrides {
    bool hasActiveSet = false;
    int  activeSet = -1;
    bool hasQuestId = false;
    int  questId = 0;
    bool hasFood = false;
    int  food = -1;
    bool hasReset = false;
    bool reset = false;
    bool hasAcceptOnly = false;
    bool acceptOnly = false;
    bool hasWyvern = false;
    bool wyvern = false;

    bool Any() const noexcept
    {
        return hasActiveSet || hasQuestId || hasFood ||
               hasReset || hasAcceptOnly || hasWyvern;
    }

    void Clear() noexcept { *this = ExternalSessionOverrides{}; }

    Config ApplyTo(const Config& base) const
    {
        Config out = base;
        if (hasActiveSet)  out.activeSet  = activeSet;
        if (hasQuestId)    out.questId    = questId;
        if (hasFood)       out.food       = food;
        if (hasReset)      out.reset      = reset;
        if (hasAcceptOnly) out.acceptOnly = acceptOnly;
        if (hasWyvern)     out.wyvern     = wyvern;
        return out;
    }

    void CommitTo(Config& base) const
    {
        if (hasActiveSet)  base.activeSet  = activeSet;
        if (hasQuestId)    base.questId    = questId;
        if (hasFood)       base.food       = food;
        if (hasReset)      base.reset      = reset;
        if (hasAcceptOnly) base.acceptOnly = acceptOnly;
        if (hasWyvern)     base.wyvern     = wyvern;
    }
};

void Init(HMODULE selfModule);
void Shutdown();

// Update [Api] Enabled/Whitelist policy; revokes any active session no longer matching.
void Configure(bool enabled, const std::string& whitelistSpec);

// Entrypoint invoked by exported __declspec(noinline) BetterINQ_AcquireApi(_ReturnAddress()).
std::int32_t AcquireFromCaller(const void* returnAddress,
                               std::uint32_t apiVersion,
                               const char* callerTag,
                               BetterINQ_Session* outSession,
                               BetterINQ_ApiV1* outApi);

// Worker-thread state publication & command queue consumption.
void PublishSnapshot(bool ready,
                     bool enabled,
                     bool characterLinked,
                     bool startupReady,
                     int saveSlot,
                     const inq::LiveContext& live,
                     const Config& effectiveCfg,
                     bool hasUnsavedExternalChanges,
                     const std::int32_t itemStoreToDisplay[81]);

std::vector<PendingCommand> TakePendingCommands();
void CompleteCommand(std::uint64_t requestId, std::int32_t result);

// Pure testable helpers
bool ComputeFileSha256Hex(const std::wstring& path, std::string& outHex);
bool MatchWhitelist(const std::string& whitelistSpec,
                    const std::string& fileNameUtf8,
                    const std::string& sha256Hex,
                    std::string* outMatchedRule = nullptr);

namespace testing {
void SetSelfModule(HMODULE selfMod);
std::int32_t AcquireForModulePath(HMODULE callerMod,
                                  const std::wstring& fullPath,
                                  std::uint32_t apiVersion,
                                  const char* callerTag,
                                  BetterINQ_Session* outSession,
                                  BetterINQ_ApiV1* outApi);
void ClearAll();
} // namespace testing

} // namespace api
