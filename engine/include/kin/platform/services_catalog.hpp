#pragma once

// The game's achievements, stats and leaderboards, as a list of its own
// (content/services.kinservices, docs/platform_services.md): each by an id the
// game chose, and by the id each store gave it where that differs (Steam's API
// names can be the game's; other stores make their own). The game asks by its
// ids; catalog_services() asks the store by the store's:
//
//     {
//       "schema": "kin.services/1",
//       "achievements": [
//         {"id": "ALL_COINS", "name": "achievement.all_coins", "description": "achievement.all_coins.how",
//          "progress": 5, "stores": {"steam": "ACH_ALL_COINS"}}
//       ],
//       "stats": [{"id": "coins", "type": "int"}],
//       "leaderboards": [{"id": "speedrun", "sort": "ascending", "display": "milliseconds"}]
//     }
//
// Names and descriptions are translation keys (content/lang), for the game's
// own lists and for tools; the store shows what is set up on it.

#include <kin/core/types.hpp>
#include <kin/platform/services.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// A store's name ("steam", as PlatformServices::backend() says it) to the id it
// knows an entry by. A store not in it knows the entry by the game's id.
using ServicesStoreIds = std::map<std::string, std::string, std::less<>>;

struct ServicesAchievement {
    std::string id;
    std::string name{};        // a translation key
    std::string description{}; // a translation key: how it is unlocked
    bool hidden = false;       // its name and description shown only once unlocked
    u32 progress = 0;          // "3 of 5": the most show_progress counts to (0: none)
    ServicesStoreIds stores{};
};

enum class ServicesStatType : u8 { Int, Float };

struct ServicesStat {
    std::string id;
    ServicesStatType type = ServicesStatType::Int;
    std::string name{}; // a translation key
    ServicesStoreIds stores{};
};

struct ServicesLeaderboard {
    std::string id;
    std::string name{}; // a translation key
    LeaderboardSort sort = LeaderboardSort::Descending;
    LeaderboardDisplay display = LeaderboardDisplay::Number;
    ServicesStoreIds stores{};
};

struct ServicesCatalog {
    std::vector<ServicesAchievement> achievements{};
    std::vector<ServicesStat> stats{};
    std::vector<ServicesLeaderboard> leaderboards{};

    // An entry by the game's id, or null.
    const ServicesAchievement* achievement(std::string_view id) const;
    const ServicesStat* stat(std::string_view id) const;
    const ServicesLeaderboard* leaderboard(std::string_view id) const;
};

// The id `store` knows an entry by: its own there, else the game's.
std::string_view store_id(const ServicesStoreIds& stores, std::string_view id, std::string_view store);

// Read from its JSON. False, with `error` (what, and where) set, when it isn't
// a catalog: not JSON, an entry without an id or with one twice, an id over 128
// characters, a type, sort or display kin doesn't know, a store's id empty.
bool parse_services_catalog(std::string_view text, ServicesCatalog& out, std::string* error = nullptr);
// From the content's pack when the game ships one (read_content_text), else from disk.
bool load_services_catalog(const std::filesystem::path& file, ServicesCatalog& out, std::string* error = nullptr);
// Its JSON, as parse_services_catalog reads it.
std::string write_services_catalog(const ServicesCatalog& catalog);

// `inner` asked by the catalog's ids: each turned into the id the store
// (inner.backend(): "steam", "offline", ...) knows it by, and back in events.
// Where the catalog lists achievements (or stats, or leaderboards), an id it
// doesn't list is refused (false, 0 or none), with a warning in the log the
// first time; where it lists none of a kind, those ids go as they are. A stat
// is set as its type says (3 for a decimal one is 3.0); a leaderboard is made
// with its sort and display; show_progress without a most counts to the
// achievement's progress. `inner` must outlive it.
std::unique_ptr<PlatformServices> catalog_services(PlatformServices& inner, ServicesCatalog catalog);

} // namespace kin
