#pragma once

#include "GlobalNamespace/BeatmapLevel.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <string>

namespace BetterSongList::PlayerStats {
    /// @brief Max score of a map with the given note count, using vanilla scoring and no modifiers.
    /// Matches 920 * notes - 7245 for maps with 13 or more notes.
    int MaxScoreForNotes(uint32_t notes);

    /// @brief Reads the local score data of the player. Has to be called on the main thread.
    /// Does nothing when a load is already running.
    void Load();
    /// @brief Starts Load() on the main thread. The returned future finishes once the data is ready, the load failed or it took too long.
    std::future<void> LoadAsync(bool needsFreshScores);
    /// @brief True once a load has completed successfully at least once.
    bool get_hasLoadedOnce();
    /// @brief True when the loaded data is up to date with the player's scores.
    bool get_isLoaded();
    /// @brief Call when a local score changed, so the next load re-reads the score data.
    void MarkScoresDirty();

    /// @brief Stores "now" as the last played time of this level and saves it to disk.
    void RecordPlay(const std::string& levelId);
    /// @brief Unix time the level was last played. 0 means it was played before this mod tracked dates. nullopt means never played.
    std::optional<int64_t> GetLastPlayed(const std::string& levelId);
    /// @brief Best accuracy (0..1) over all played difficulties of the level, nullopt if no valid score exists or the max score is unknown.
    std::optional<float> GetBestAccuracy(GlobalNamespace::BeatmapLevel* level);
}
