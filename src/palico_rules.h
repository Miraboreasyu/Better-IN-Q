#pragma once
#include <array>
#include <cstdint>

// Reconstructed from speedrun_util 0.1.0-beta-8b8f3fddedfe, selector
// RVA 0x45d90 and candidate builder 0x1e0c0. No game memory access.
namespace palico {
constexpr int kSongCount = 16;
struct Song { int gameId, slot, minimumParameter; const char* key; };
const std::array<Song, kSongCount>& Songs();
int SongIndex(int gameId);
// Resource entries are per-level costs. Return false rather than overflowing.
bool ExperienceForLevel(const int* costs, int count, int level, int& experience);
int LevelForExperience(const int* costs, int count, int experience);
struct Policy {
    std::array<bool, kSongCount> banned{};
    std::array<int, kSongCount> rank{}; // Positive lower rank wins; zero = no preference.
    int firstSong = -1;
    bool skipAllBanned = false;
};
struct Request { int slot = 1; std::uint32_t candidates = 0; int forced = -1, original = -1; };
// Reset on the loading edge, even when repeating the SAME quest ID.
class QuestCycle {
    bool loading_ = false;
public:
    bool Sample(bool known, bool loading) {
        if (!known) return false;
        const bool start = loading && !loading_;
        loading_ = loading;
        return start;
    }
};
struct Choice { int index = -1; bool legal = true; };
Choice Select(const Policy&, const Request&, bool first, std::uint32_t random);
struct Snapshot {
    int parameter[2] = {0, 0};
    bool metadata[2] = {false, false};
    unsigned short cooldown[2] = {0, 0};
    std::uint32_t flags = 0, status = 0;
    std::array<float, 30> timers{};
    float healthConditionA = 0, healthConditionB = 0;
};
Request Candidates(const Snapshot&, int slot, int originalGameId);
} // namespace palico
