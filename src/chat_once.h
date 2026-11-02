#pragma once

#include "log.h"

#include <atomic>
#include <cstdint>

// Stable semantic categories, not rendered text, IDs, or the current language.
// Do not reset these notices on cancellation, slot changes, or Dormant transitions.
enum class ChatNotice {
    UnsafeEquip,
    MissingEquip,
    MissingItem,
    CoreUnavailable,
    MissingActiveSet,
    EmptyItem,
    InvalidEquip,
    QuestAcceptFailed,
    CannotRestart,
    CloseWindows,
    NoQuest,
    PanelUnavailable,
    Ready,
    Count
};

inline void ChatOnce(ChatNotice notice, const std::string& text)
{
    static_assert(static_cast<unsigned>(ChatNotice::Count) <= 64,
                  "ChatOnce categories must fit in the process-lifetime bit mask");
    const auto index = static_cast<unsigned>(notice);
    if (index >= static_cast<unsigned>(ChatNotice::Count)) return;
    const auto bit = std::uint64_t{1} << index;
    // An external-linkage inline function shares this local static across TUs.
    // Only the winning caller submits to Chat; no ordering of other data is needed.
    static std::atomic<std::uint64_t> sent{0};
    if ((sent.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) Chat(text);
}
