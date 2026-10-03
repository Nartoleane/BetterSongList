#include "Utils/PlayerStats.hpp"

#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerLevelStatsData.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapBasicData.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapDifficulty.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/StandardLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/LevelCompletionResults.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include "Utils/SongDetails.hpp"
#include "hooking.hpp"
#include "logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// defined in LocalScoresUtils.cpp
namespace BetterSongList::LocalScoresUtils {
    GlobalNamespace::PlayerDataModel* get_playerDataModel();
}

namespace BetterSongList::PlayerStats {
    struct DifficultyStats {
        GlobalNamespace::BeatmapCharacteristicSO* characteristic;
        int32_t difficulty;
        int32_t highScore;
        bool validScore;
    };

    struct LevelStats {
        bool played = false;
        std::vector<DifficultyStats> difficulties;
    };

    // Guards playTimes and snapshot
    static std::shared_mutex dataMutex;
    // levelID -> unix time of the last play, saved to disk
    static std::unordered_map<std::string, int64_t> playTimes;
    // levelID -> data copied from the game's local score data
    static std::unordered_map<std::string, LevelStats> snapshot;

    static std::once_flag playTimesFileLoaded;
    static std::mutex saveMutex;

    static std::atomic<bool> loadedOnce = false;
    static std::atomic<bool> dirty = true;
    static std::atomic<bool> loading = false;
    // Counts every Load() call, so waiting for a load can tell that its attempt has finished
    static std::atomic<int> loadAttempts = 0;

    int MaxScoreForNotes(uint32_t notes) {
        if (notes <= 13) {
            int64_t total = 0;
            for (uint32_t i = 1; i <= notes; i++) {
                total += i == 1 ? 1 : (i <= 5 ? 2 : 4);
            }
            return static_cast<int>(115 * total);
        } else {
            return static_cast<int>((notes - 13) * 1127.2 + 5705);
        }
    }

    static std::filesystem::path GetPlayTimesPath() {
        return std::filesystem::path(getDataDir(MOD_ID)) / "lastplayed.txt";
    }

    static void LoadPlayTimesFile() {
        std::ifstream in(GetPlayTimesPath());
        if (!in) return;

        std::unordered_map<std::string, int64_t> loaded;
        std::string line;
        while (std::getline(in, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
            try {
                loaded[line.substr(tab + 1)] = std::stoll(line.substr(0, tab));
            } catch (...) {
                continue;
            }
        }

        std::unique_lock lock(dataMutex);
        for (auto& [id, time] : loaded) {
            auto& existing = playTimes[id];
            existing = std::max(existing, time);
        }
        INFO("Loaded {} last played dates", loaded.size());
    }

    static void SavePlayTimesFile() {
        std::lock_guard saveLock(saveMutex);
        std::ostringstream out;
        {
            std::shared_lock lock(dataMutex);
            for (auto& [id, time] : playTimes) out << time << '\t' << id << '\n';
        }

        try {
            auto path = GetPlayTimesPath();
            std::filesystem::create_directories(path.parent_path());
            // Write to a temporary file first, so a crash can't leave a half written file behind
            auto tempPath = path;
            tempPath += ".tmp";
            {
                std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
                file << out.str();
                file.flush();
                if (!file) throw std::runtime_error("Could not write file");
            }
            std::filesystem::rename(tempPath, path);
        } catch (std::exception const& error) {
            ERROR("Failed to save last played dates: {}", error.what());
        }
    }

    void RecordPlay(const std::string& levelId) {
        if (levelId.empty()) return;
        std::call_once(playTimesFileLoaded, LoadPlayTimesFile);
        {
            std::unique_lock lock(dataMutex);
            playTimes[levelId] = static_cast<int64_t>(std::time(nullptr));
        }
        SavePlayTimesFile();
    }

    bool get_hasLoadedOnce() {
        return loadedOnce;
    }

    bool get_isLoaded() {
        return loadedOnce && !dirty && !loading;
    }

    void MarkScoresDirty() {
        dirty = true;
    }

    void Load() {
        loadAttempts++;
        if (loading) return;

        auto* model = LocalScoresUtils::get_playerDataModel();
        auto* playerData = model ? model->_playerData : nullptr;
        if (!playerData) {
            WARNING("PlayerStats::Load() => No player data found, cannot load local scores");
            return;
        }

        std::call_once(playTimesFileLoaded, LoadPlayTimesFile);

        loading = true;
        // Anything changing after this point makes the data outdated again
        dirty = false;

        il2cpp_utils::il2cpp_aware_thread([playerData]() {
            try {
                auto levelData = ListW<GlobalNamespace::PlayerLevelStatsData*>::New();
                auto* levelStats = playerData->get_levelsStatsData();
                if (!levelStats) throw std::runtime_error("Local score data is not available");
                auto stats = levelStats->get_Values()->i___System__Collections__Generic__IEnumerable_1_TValue_();
                levelData->AddRange(stats);

                std::unordered_map<std::string, LevelStats> fresh;
                for (auto x : levelData) {
                    if (!x) continue;
                    StringW id = x->_levelID;
                    if (!id) continue;
                    auto& level = fresh[static_cast<std::string>(id)];
                    if (x->_playCount > 0 || x->_validScore) level.played = true;
                    level.difficulties.push_back({
                        x->_beatmapCharacteristic,
                        static_cast<int32_t>(x->_difficulty),
                        x->_highScore,
                        static_cast<bool>(x->_validScore)
                    });
                }

                {
                    std::unique_lock lock(dataMutex);
                    snapshot = std::move(fresh);
                }
                loadedOnce = true;
                loading = false;
            } catch (...) {
                ERROR("PlayerStats::Load() => Exception during loading local scores");
                dirty = true;
                loading = false;
            }
        }).detach();
    }

    std::future<void> LoadAsync(bool needsFreshScores) {
        const int startAttempts = loadAttempts;
        BSML::MainThreadScheduler::Schedule(Load);

        return std::async(std::launch::deferred, [needsFreshScores, startAttempts]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline) {
                if (needsFreshScores ? get_isLoaded() : get_hasLoadedOnce()) return;
                // The load this call asked for was attempted and is over, but did not succeed
                if (!loading && loadAttempts > startAttempts) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            WARNING("PlayerStats: timed out waiting for local scores");
        });
    }

    std::optional<int64_t> GetLastPlayed(const std::string& levelId) {
        if (levelId.empty()) return std::nullopt;
        std::shared_lock lock(dataMutex);
        if (auto itr = playTimes.find(levelId); itr != playTimes.end()) return itr->second;
        // Played before this mod started tracking dates
        if (auto itr = snapshot.find(levelId); itr != snapshot.end() && itr->second.played) return 0;
        return std::nullopt;
    }

    static std::string HashFromLevelId(const std::string& levelId) {
        if (levelId.size() < 53 || levelId[12] != '_') return "";
        return levelId.substr(13, 40);
    }

    // Note count of a difficulty. The game's own level data is used first, which covers every song including
    // the base game ones. song-details is the fallback for custom levels that report no note count.
    static uint32_t GetNoteCount(GlobalNamespace::BeatmapLevel* level, const std::string& levelId, const DifficultyStats& stats) {
        if (!stats.characteristic) return 0;

        try {
            auto* data = level->GetDifficultyBeatmapData(stats.characteristic, GlobalNamespace::BeatmapDifficulty(stats.difficulty));
            if (data && data->notesCount > 0) return static_cast<uint32_t>(data->notesCount);
        } catch (...) {
            // Difficulty is not part of this level, try the fallback
        }

        auto* details = SongDetails::get_songDetails();
        if (!details || !details->songs.get_isDataAvailable() || details->songs.size() == 0) return 0;

        auto hash = HashFromLevelId(levelId);
        if (hash.empty()) return 0;
        auto& song = details->songs.FindByHash(hash);
        if (song == SongDetailsCache::Song::none) return 0;

        auto characteristic = SongDetails::BeatmapCharacteristicToBeatStarCharacteristic(stats.characteristic);
        auto& difficulty = song.GetDifficulty(static_cast<SongDetailsCache::MapDifficulty>(stats.difficulty), characteristic);
        if (difficulty == SongDetailsCache::SongDifficulty::none) return 0;
        return difficulty.notes;
    }

    std::optional<float> GetBestAccuracy(GlobalNamespace::BeatmapLevel* level) {
        if (!level) return std::nullopt;
        StringW id = level->___levelID;
        if (!id) return std::nullopt;
        const auto levelId = static_cast<std::string>(id);

        std::vector<DifficultyStats> difficulties;
        {
            std::shared_lock lock(dataMutex);
            auto itr = snapshot.find(levelId);
            if (itr == snapshot.end()) return std::nullopt;
            difficulties = itr->second.difficulties;
        }

        std::optional<float> best;
        for (auto& stats : difficulties) {
            if (!stats.validScore || stats.highScore <= 0) continue;

            int maxScore = MaxScoreForNotes(GetNoteCount(level, levelId, stats));
            if (maxScore <= 0) continue;

            // Scores set with positive modifiers can go over the max score of the map
            float accuracy = std::min(1.0f, static_cast<float>(stats.highScore) / static_cast<float>(maxScore));
            if (!best || accuracy > *best) best = accuracy;
        }
        return best;
    }
}

// Called whenever a song ends (finished, failed or quit). IncreaseNumberOfGameplays is too small to be hooked.
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_HandleMainGameSceneDidFinish,
    &GlobalNamespace::MenuTransitionsHelper::HandleMainGameSceneDidFinish,
    void,
    GlobalNamespace::MenuTransitionsHelper* self,
    GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* standardLevelScenesTransitionSetupData,
    GlobalNamespace::LevelCompletionResults* levelCompletionResults
) {
    try {
        if (standardLevelScenesTransitionSetupData) {
            GlobalNamespace::BeatmapKey key = standardLevelScenesTransitionSetupData->beatmapKey;
            StringW id = key.levelId;
            if (id) BetterSongList::PlayerStats::RecordPlay(static_cast<std::string>(id));
        }
    } catch (...) {
        ERROR("Failed to record last played date");
    }
    MenuTransitionsHelper_HandleMainGameSceneDidFinish(self, standardLevelScenesTransitionSetupData, levelCompletionResults);
}
