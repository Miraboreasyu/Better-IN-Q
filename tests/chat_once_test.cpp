// Standalone test: link this file with chat_once_other.cpp; no game dependencies.
#include "../src/chat_once.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

void EmitNoticeFromOtherTU();

static std::mutex chatMutex;
static std::vector<std::string> chats;
void Chat(const std::string& text)
{
    std::lock_guard<std::mutex> lock(chatMutex);
    chats.push_back(text);
}

static int failures = 0;
#define CHECK(c, label) do { if (!(c)) { std::printf("FAIL: %s\n", label); ++failures; } } while (0)

int main()
{
    ChatOnce(ChatNotice::Ready, "ready from the first translation unit");
    EmitNoticeFromOtherTU();
    CHECK(chats.size() == 1, "inline local static is shared across translation units");

    ChatOnce(ChatNotice::MissingEquip, "loadout 1 is missing");
    ChatOnce(ChatNotice::MissingEquip, "loadout 224 is missing");
    ChatOnce(ChatNotice::MissingEquip, "装備セットが見つかりません");
    ChatOnce(ChatNotice::MissingEquip, "装备套装不存在");
    CHECK(chats.size() == 2 && chats.back() == "loadout 1 is missing",
          "dynamic IDs and language changes cannot bypass category deduplication");
    ChatOnce(ChatNotice::MissingItem, "loadout 1 is missing");
    CHECK(chats.size() == 3, "identical text in different categories is independent");

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 24; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int j = 0; j < 200; ++j) {
                ChatOnce(ChatNotice::CoreUnavailable, "core " + std::to_string(i));
                ChatOnce(ChatNotice::PanelUnavailable, "panel " + std::to_string(j));
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    CHECK(chats.size() == 5, "concurrent callers emit exactly once for each category");

    for (unsigned i = 0; i < static_cast<unsigned>(ChatNotice::Count); ++i)
        ChatOnce(static_cast<ChatNotice>(i), "all categories");
    CHECK(chats.size() == static_cast<unsigned>(ChatNotice::Count), "every category gets an independent bit");
    ChatOnce(ChatNotice::Count, "sentinel is not a category");
    ChatOnce(static_cast<ChatNotice>(-1), "invalid category");
    CHECK(chats.size() == static_cast<unsigned>(ChatNotice::Count), "invalid categories do not shift or emit");

    const auto before = chats.size();
    Chat("operation completed"); Chat("operation completed");
    Chat("watchdog recovered"); Chat("watchdog recovered");
    CHECK(chats.size() == before + 4, "ordinary Chat remains repeatable for results and recovery");
    std::printf("chat_once_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
