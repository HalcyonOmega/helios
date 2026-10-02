/**
 * @file src/steam_library.h
 * @brief Declarations for discovering installed Steam games.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

namespace steam_library {
  /**
   * @brief An installed Steam game that can be launched through the Steam client.
   */
  struct game_t {
    std::string appid;  ///< Steam application id.
    std::string name;  ///< Display name from the app manifest.
    std::string image_path;  ///< Portrait cover art from Steam's library cache, or empty.
  };

  /**
   * @brief List fully installed games across every Steam library on this machine.
   *
   * Steam tools (Proton, the Steam Linux Runtime, redistributables) are skipped. Results are sorted
   * by name. Returns an empty list when Steam is not installed or on unsupported platforms.
   *
   * @return Installed games.
   */
  std::vector<game_t> installed_games();

  /**
   * @brief Cheap summary of the Steam library state.
   *
   * Changes whenever a game is installed, removed or updated, so callers can skip re-scanning.
   *
   * @return Opaque fingerprint string.
   */
  std::string fingerprint();

  /**
   * @brief Steam's launch command for an installed game.
   *
   * @param appid Steam application id.
   * @return Command that asks the running (or a new) Steam client to start the game.
   */
  std::string launch_command(const std::string &appid);
}  // namespace steam_library
