#include "Sorters/Models/PlayerStatsSorter.hpp"

#include "Utils/PlayerStats.hpp"
#include "Utils/SongDetails.hpp"
#include "Utils/SongListLegendBuilder.hpp"
#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace BetterSongList {
    PlayerStatsSorter::PlayerStatsSorter(ValueGetterFunc valueFunc, LegendGetterFunc legendFunc, bool needsFreshScores, bool needsSongDetails)
        : ISorterWithLegend(), ISorterCustom(),
          valueGetter(std::move(valueFunc)), legendGetter(std::move(legendFunc)),
          needsFreshScores(needsFreshScores), needsSongDetails(needsSongDetails) {}

    bool PlayerStatsSorter::get_isReady() const {
        if (needsFreshScores ? !PlayerStats::get_isLoaded() : !PlayerStats::get_hasLoadedOnce()) return false;
        if (needsSongDetails && !SongDetails::get_finishedInitAttempt()) return false;
        return true;
    }

    std::future<void> PlayerStatsSorter::Prepare() {
        // Called on a background thread. Reading the scores is started on the main thread and waited for here.
        return std::async(std::launch::deferred, [load = PlayerStats::LoadAsync(needsFreshScores), needsSongDetails = needsSongDetails]() mutable {
            load.get();

            if (needsSongDetails) {
                // song-details only fills in missing note counts, so a failure to load it is not an error here
                SongDetails::Init();
                while (!SongDetails::get_finishedInitAttempt()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }

    ISorterWithLegend::Legend PlayerStatsSorter::BuildLegend(ArrayW<GlobalNamespace::BeatmapLevel*> levels) const {
        return SongListLegendBuilder::BuildFor(levels, [this](GlobalNamespace::BeatmapLevel* level) -> std::string {
            return legendGetter(valueGetter(level));
        });
    }

    void PlayerStatsSorter::DoSort(ArrayW<GlobalNamespace::BeatmapLevel*>& levels, bool ascending) const {
        // Work out every value once, the getters are not free
        std::vector<std::pair<GlobalNamespace::BeatmapLevel*, double>> withValue;
        std::vector<GlobalNamespace::BeatmapLevel*> withoutValue;
        withValue.reserve(levels.size());

        for (auto* level : levels) {
            auto value = valueGetter(level);
            if (value.has_value()) withValue.emplace_back(level, *value);
            else withoutValue.push_back(level);
        }

        // Best value first by default, flipping the direction shows the lowest first. Ties keep the order they had before.
        std::stable_sort(withValue.begin(), withValue.end(), [ascending](const auto& lhs, const auto& rhs) {
            return ascending ? lhs.second < rhs.second : lhs.second > rhs.second;
        });

        size_t index = 0;
        for (auto& [level, value] : withValue) levels[index++] = level;
        for (auto* level : withoutValue) levels[index++] = level;
    }
}
