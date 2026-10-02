/**
 * @file src/steam_library.cpp
 * @brief Definitions for discovering installed Steam games.
 */
// header include
#include "steam_library.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

// local includes
#include "logging.h"

namespace steam_library {
  namespace fs = std::filesystem;
  using namespace std::literals;

  namespace {
    /**
     * @brief Steam's "fully installed" bit in an app manifest's StateFlags.
     */
    constexpr unsigned long STATE_FULLY_INSTALLED = 4;

    /**
     * @brief Display names of Steam tools that show up as installed apps but are not games.
     */
    constexpr std::array<std::string_view, 5> TOOL_PREFIXES {
      "Proton"sv,
      "Steam Linux Runtime"sv,
      "Steamworks Common Redistributables"sv,
      "SteamVR"sv,
      "Steam Audio"sv,
    };

    /**
     * @brief Cover art file names in order of preference (portrait box art first).
     */
    constexpr std::array<std::string_view, 6> COVER_NAMES {
      "library_600x900.jpg"sv,
      "library_600x900.png"sv,
      "library_capsule.jpg"sv,
      "library_capsule.png"sv,
      "header.jpg"sv,
      "library_header.jpg"sv,
    };

    std::optional<fs::path> canonical_dir(const fs::path &path) {
      std::error_code ec;
      auto resolved = fs::canonical(path, ec);
      if (ec || !fs::is_directory(resolved, ec)) {
        return std::nullopt;
      }
      return resolved;
    }

    /**
     * @brief Steam install roots for native and Flatpak Steam.
     */
    std::vector<fs::path> steam_roots() {
#ifdef __linux__
      const char *home = std::getenv("HOME");
      if (!home || !*home) {
        return {};
      }

      const fs::path home_dir {home};
      const std::array candidates {
        home_dir / ".local/share/Steam",
        home_dir / ".steam/steam",
        home_dir / ".steam/root",
        home_dir / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
      };

      std::vector<fs::path> roots;
      for (const auto &candidate : candidates) {
        auto root = canonical_dir(candidate);
        if (root && canonical_dir(*root / "steamapps") && std::ranges::find(roots, *root) == roots.end()) {
          roots.emplace_back(std::move(*root));
        }
      }
      return roots;
#else
      return {};
#endif
    }

    /**
     * @brief Undo VDF string escaping (`\\` and `\"`).
     */
    std::string unescape_vdf(const std::string &value) {
      std::string out;
      out.reserve(value.size());
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
          ++i;
        }
        out += value[i];
      }
      return out;
    }

    /**
     * @brief First value for each quoted key in a VDF/ACF file, in file order.
     *
     * App manifests list the top-level keys (appid, name, StateFlags, ...) before any nested
     * block, so the first occurrence of a key is the top-level one.
     */
    std::map<std::string, std::string> read_first_values(const fs::path &path) {
      static const std::regex key_value {R"re(^\s*"([^"]+)"\s+"((?:[^"\\]|\\.)*)"\s*$)re"};

      std::map<std::string, std::string> values;
      std::ifstream file(path);
      std::string line;
      std::smatch match;
      while (std::getline(file, line)) {
        if (std::regex_match(line, match, key_value)) {
          values.try_emplace(match[1].str(), unescape_vdf(match[2].str()));
        }
      }
      return values;
    }

    /**
     * @brief All library folders of a Steam root, including the root itself.
     */
    std::vector<fs::path> library_dirs(const fs::path &root) {
      static const std::regex path_line {R"re(^\s*"path"\s+"((?:[^"\\]|\\.)*)"\s*$)re"};

      std::vector<fs::path> dirs {root};
      std::ifstream file(root / "steamapps/libraryfolders.vdf");
      std::string line;
      std::smatch match;
      while (std::getline(file, line)) {
        if (!std::regex_match(line, match, path_line)) {
          continue;
        }
        auto dir = canonical_dir(unescape_vdf(match[1].str()));
        if (dir && std::ranges::find(dirs, *dir) == dirs.end()) {
          dirs.emplace_back(std::move(*dir));
        }
      }
      return dirs;
    }

    std::vector<fs::path> manifests(const fs::path &library) {
      std::vector<fs::path> files;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(library / "steamapps", ec)) {
        const auto file_name = entry.path().filename().string();
        if (file_name.starts_with("appmanifest_") && file_name.ends_with(".acf")) {
          files.emplace_back(entry.path());
        }
      }
      std::ranges::sort(files);
      return files;
    }

    bool is_tool(const std::string &name) {
      return std::ranges::any_of(TOOL_PREFIXES, [&name](std::string_view prefix) {
        return name.starts_with(prefix);
      });
    }

    /**
     * @brief Best cached cover image for an app, or an empty string.
     *
     * Current Steam clients keep art under `librarycache/<appid>/` (sometimes one hashed
     * directory deeper); older clients used flat `librarycache/<appid>_<name>` files.
     */
    std::string cover_art(const fs::path &root, const std::string &appid) {
      const auto cache = root / "appcache/librarycache";
      std::error_code ec;

      std::map<std::string, fs::path> found;
      const auto app_dir = cache / appid;
      if (fs::is_directory(app_dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(app_dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
          if (it.depth() > 1) {
            it.disable_recursion_pending();
            continue;
          }
          if (it->is_regular_file(ec)) {
            found.try_emplace(it->path().filename().string(), it->path());
          }
        }
      }

      for (const auto &name : COVER_NAMES) {
        if (auto it = found.find(std::string(name)); it != found.end()) {
          return it->second.string();
        }
        if (auto flat = cache / (appid + "_" + std::string(name)); fs::is_regular_file(flat, ec)) {
          return flat.string();
        }
      }
      return {};
    }
  }  // namespace

  std::vector<game_t> installed_games() {
    std::vector<game_t> games;
    std::set<std::string> seen;

    for (const auto &root : steam_roots()) {
      for (const auto &library : library_dirs(root)) {
        for (const auto &manifest : manifests(library)) {
          auto values = read_first_values(manifest);
          const auto &appid = values["appid"];
          const auto &name = values["name"];
          if (appid.empty() || name.empty() || is_tool(name) || seen.contains(appid)) {
            continue;
          }

          unsigned long state = 0;
          try {
            state = std::stoul(values["StateFlags"]);
          } catch (const std::exception &) {
            continue;
          }
          if (!(state & STATE_FULLY_INSTALLED)) {
            continue;
          }

          seen.insert(appid);
          games.push_back(game_t {appid, name, cover_art(root, appid)});
        }
      }
    }

    std::ranges::sort(games, {}, &game_t::name);
    return games;
  }

  std::string fingerprint() {
    std::ostringstream summary;
    std::error_code ec;
    for (const auto &root : steam_roots()) {
      for (const auto &library : library_dirs(root)) {
        for (const auto &manifest : manifests(library)) {
          summary << manifest.string() << ':'
                  << fs::last_write_time(manifest, ec).time_since_epoch().count() << ';';
        }
      }
    }
    return summary.str();
  }

  std::string launch_command(const std::string &appid) {
    return "steam steam://rungameid/" + appid;
  }
}  // namespace steam_library
