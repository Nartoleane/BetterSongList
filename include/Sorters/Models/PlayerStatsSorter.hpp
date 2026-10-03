#pragma once

#include "ISorter.hpp"

#include <functional>
#include <optional>
#include <string>

namespace BetterSongList {
    /// @brief Sorter for values that come from the player's own data (last played date, accuracy).
    /// Maps without a value are always listed last. The best value comes first unless the sort direction is flipped.
    class PlayerStatsSorter : public ISorterWithLegend, public ISorterCustom {
        public:
            using ValueGetterFunc = std::function<std::optional<double>(GlobalNamespace::BeatmapLevel*)>;
            using LegendGetterFunc = std::function<std::string(std::optional<double>)>;

            /// @param valueFunc gets the value to sort a level by
            /// @param legendFunc gets the legend text for a value (nullopt = the level has no value)
            /// @param needsFreshScores true if the values depend on the local scores, so they have to be reloaded after a score changed
            /// @param needsSongDetails true if the values use song-details when it is available
            PlayerStatsSorter(ValueGetterFunc valueFunc, LegendGetterFunc legendFunc, bool needsFreshScores, bool needsSongDetails);

            virtual bool get_isReady() const override;
            virtual std::future<void> Prepare() override;
            virtual Legend BuildLegend(ArrayW<GlobalNamespace::BeatmapLevel*> levels) const override;
            virtual void DoSort(ArrayW<GlobalNamespace::BeatmapLevel*>& levels, bool ascending) const override;
        private:
            ValueGetterFunc valueGetter;
            LegendGetterFunc legendGetter;
            bool needsFreshScores;
            bool needsSongDetails;
    };
}
