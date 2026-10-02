/**
 * @file src/platform/linux/kwingrab.cpp
 * @brief KWin direct ScreenCast capture via zkde_screencast_unstable_v1 Wayland protocol.
 *
 * Bypasses xdg-desktop-portal entirely. Sunshine connects directly to KWin's
 * Wayland protocol to obtain a PipeWire node_id, then streams frames via PipeWire.
 *
 * Chain: KWin -> Wayland kde_screencast -> PipeWire -> Sunshine
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <pwd.h>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>

// lib includes
#include <gio/gio.h>
#include <lizardbyte/common/env.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol header
#include <kde-output-order-v1.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "pipewire.cpp"
#include "src/config.h"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace kwin {
  /**
   * KWin Wayland ScreenCast permissions
   *
   * To have access to zkde_screencast_unstable_v1 KWin checks for a .desktop file with
   * X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1 and the current executable name
   * in the Exec= parameter.
   */
  class screencast_permission_helper_t {
  public:
    /**
     * @brief Check whether permission system deactivated.
     *
     * @return True when KWin reports that the permission system is disabled.
     */
    static bool is_permission_system_deactivated() {
      return lizardbyte::common::get_env("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1";
    }

    /**
     * @brief Configure the KWin screencast session.
     */
    static void setup() {
      if (initialized) {
        return;
      }

      // System: Check system XDG applications for permission (usually installed with Sunshine)
      if (check_kwin_system_permissions(filename_prefix(), get_executable_full_path())) {
        create_file = false;
        using_system_permission = true;
        initialized = true;
        return;
      }

      // If we do not have a system permission, check if we need a temporary permission via user's application directory
      if (is_permission_system_deactivated()) {
        BOOST_LOG(info) << "[kwingrab] No permission desktop file necessary. KWin permission system deactivated.";
        create_file = false;
        initialized = true;
        return;
      }

      install_user_permission();
      initialized = true;
    }

    /**
     * @brief Switch to a per-user permission file after KWin ignored the system one.
     *
     * KWin only honours desktop files that are in KDE's service cache. After a NixOS switch the
     * system file lives under a new store path that the running session has not indexed yet,
     * while files in the user's applications directory are picked up within seconds.
     *
     * @return True when a per-user permission file is now in place and a retry is worthwhile.
     */
    static bool fall_back_to_user_permission() {
      if (!using_system_permission || is_permission_system_deactivated()) {
        return false;
      }
      using_system_permission = false;
      BOOST_LOG(info) << "[kwingrab] KWin did not honour the system permission file yet; installing a per-user one";
      create_file = true;
      return install_user_permission();
    }

  private:
    static std::string filename_prefix() {
      return std::format("{}.kwin", PROJECT_FQDN);
    }

    /**
     * @brief Ensure the user's applications directory holds a permission file for this binary.
     *
     * @return True when a matching per-user file exists (pre-existing or newly written).
     */
    static bool install_user_permission() {
      const auto executablepath = get_executable_full_path();

      // User: Check and (if necessary) update user's XDG applications for permission
      auto user_applications = get_xdg_user_applications_path();
      if (user_applications.empty()) {
        BOOST_LOG(error) << "[kwingrab] Failed to determine user application directory. Cannot continue with permission setup.";
        return false;
      }
      // Create non-existing application directory so we can write into it
      if (!std::filesystem::exists(user_applications) && !std::filesystem::create_directories(user_applications)) {
        // In case of failure log and return
        BOOST_LOG(error) << "[kwingrab] Failed to create application directory. Cannot continue with permission setup.";
        create_file = false;
        return false;
      }
      auto user_filepathprefix = (std::filesystem::path(user_applications) / filename_prefix()).string();
      for (const auto &path : std::filesystem::directory_iterator(user_applications)) {
        // List existing files for prefix and check if they contain this executable or remove them
        const auto entry = path.path().string();
        if (entry.starts_with(user_filepathprefix)) {
          auto entry_executablepath = get_executable_from_desktop_file(entry);
          if (!entry_executablepath.empty() && entry_executablepath == executablepath) {
            // This entry is exactly the one we need
            BOOST_LOG(debug) << "[kwingrab] Ignoring current temporary KWin wayland permission file: "sv << entry;
            create_file = false;
            continue;
          }
          if (!entry_executablepath.empty() && std::filesystem::exists(entry_executablepath)) {
            // This entry is for another sunshine executable that still exists
            BOOST_LOG(debug) << "[kwingrab] Ignoring other valid temporary KWin wayland permission file: "sv << entry;
            continue;
          }
          if (std::filesystem::remove(path)) {
            BOOST_LOG(info) << "[kwingrab] Removed stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          } else {
            BOOST_LOG(warning) << "[kwingrab] Failed to remove stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          }
        }
      }
      if (!create_file) {
        return true;
      }

      // Generate a unique file identifier based on current unixtime
      auto user_filepathidentifier = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
      auto user_filepath = std::format("{}{}.desktop", user_filepathprefix, user_filepathidentifier);
      // Write new file if necessary
      std::ofstream filestream(user_filepath);
      if (!filestream.is_open()) {
        BOOST_LOG(warning) << "[kwingrab] Failed to open temporary KWin wayland permission file: "sv << user_filepath;
        return false;
      }
      filestream << "[Desktop Entry]" << std::endl
                 << "Exec=" << executablepath << std::endl
                 << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1" << std::endl
                 << "Type=Application" << std::endl
                 << "Name="sv << PROJECT_FQDN << "-kwin-wayland-permission" << std::endl
                 << "Comment=Sunshine KWin screencast permission" << std::endl
                 << "NoDisplay=true" << std::endl;
      filestream.close();
      // Give KWin time to catch up to the new desktop file
      BOOST_LOG(info) << "[kwingrab] Created temporary KWin wayland permission file: "sv << user_filepath << " - Waiting 3 seconds for KDE to pick up new file.";
      std::this_thread::sleep_for(std::chrono::milliseconds(3000));
      return true;
    }

  public:
    /**
     * @brief Check whether newly initialized.
     *
     * @return True when KWin was initialized during this check.
     */
    static bool is_newly_initialized() {
      return create_file;
    }

  private:
    static inline bool initialized = false;
    static inline bool create_file = true;
    static inline bool using_system_permission = false;

    static std::filesystem::path get_home_dir() {
      // Check HOME environment variable
      if (std::string homedir = lizardbyte::common::get_env("HOME"); !homedir.empty()) {
        return homedir;
      }
      // Fall back to home directory from NSS passwd
      // Note: This should be thread-safe as we're always accessing the same entry for Sunshine
      return getpwuid(geteuid())->pw_dir;
    }

    static std::filesystem::path get_xdg_user_applications_path() {
      // Follow the XDG base directory specification for user data home:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      std::filesystem::path xdg_data_home;
      if (std::string dir = lizardbyte::common::get_env("XDG_DATA_HOME"); !dir.empty()) {
        xdg_data_home = std::filesystem::path(dir);
      } else {
        const auto homedir = get_home_dir();
        if (homedir.empty()) {
          return "";
        }
        xdg_data_home = std::filesystem::path(homedir) / ".local"sv / "share"sv;
      }
      return xdg_data_home / "applications";
    }

    static std::string get_executable_full_path() {
      // Adapted from https://linuxvox.com/blog/how-do-i-find-the-location-of-the-executable-in-c/
      constexpr auto path_len = PATH_MAX;  // PATH_MAX is defined in limits.h (e.g., 4096 on Linux)
      auto path_exe = std::make_unique<char[]>(path_len);
      // Read the symlink /proc/self/exe into path_exe
      const ssize_t len = readlink("/proc/self/exe", &path_exe[0], path_len - 1);
      if (len == -1) {
        return "";
      }
      // Return path_exe as a proper std::string with len returned by readlink
      return std::string(path_exe.get(), len);
    }

    static std::string get_executable_from_desktop_file(const std::string &path) {
      if (std::ifstream file(path); file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Exec=") && line.length() > 5) {
            return line.substr(5);
          }
        }
      }
      return "";
    }

    static bool check_kwin_system_permissions(const std::string_view &filenameprefix, const std::string_view &executablepath) {
      // Find data dirs to check from XDG_DATA_DIRS
      std::vector<std::string> xdg_data_dirs;
      if (const std::string e = lizardbyte::common::get_env("XDG_DATA_DIRS"); !e.empty()) {
        std::stringstream ss(e);
        std::string item;

        while (getline(ss, item, ':')) {  // : is likely valid for all OSes supported, if a constant is available it should be used instead
          xdg_data_dirs.push_back(item);
        }
      }
      // Use defaults from https://specifications.freedesktop.org/basedir/latest/ if ENV var was empty
      if (xdg_data_dirs.empty()) {
        xdg_data_dirs.emplace_back("/usr/local/share/");
        xdg_data_dirs.emplace_back("/usr/share/");
      }
      // Check for ${filenameprefix}.desktop in each directory
      for (auto const &dir : xdg_data_dirs) {
        std::string filename = std::format("{0}{1}applications{1}{2}.desktop", dir, boost::filesystem::path::preferred_separator, filenameprefix);
        if (std::filesystem::exists(filename)) {
          auto file_executablepath = get_executable_from_desktop_file(filename);
          if (file_executablepath == executablepath) {
            BOOST_LOG(info) << "[kwingrab] Found matching system KWin desktop permission file: "sv << filename;
            return true;
          }
        }
      }
      return false;
    }
  };

  // Output parameters
  /**
   * @brief KWin screencast output name and geometry.
   */
  struct output_parameter_t {
    std::string name;  ///< KWin output name.
    int width = 0;  ///< Output width in pixels.
    int height = 0;  ///< Output height in pixels.
    int pos_x = 0;  ///< Output X position in the compositor layout.
    int pos_y = 0;  ///< Output Y position in the compositor layout.
    // order is needed to get a sorted output list and should be updated before sorting to have current values
    /**
     * @brief Order.
     */
    size_t order = SIZE_MAX;  // Use high number to keep monitors with uninitialized order value to the back
  };

  /**
   * Wayland KDE ScreenCast session
   *
   * Owns its own wl_display connection. Binds zkde_screencast_unstable_v1
   * and wl_output from the registry, then calls stream_output() to start
   * a ScreenCast. Waits for the created(node_id) event from KWin.
   */
  class screencast_t {
  public:
    screencast_t &operator=(screencast_t &&) = delete;  // Do not allow to copying

    ~screencast_t() {
      disconnect();
    }

    /**
     * @brief Release every Wayland object and close the connection.
     */
    void disconnect() {
      // Release KDE screencast wayland extensions and reset pointers
      if (kde_screencast_stream_v1_) {
        zkde_screencast_stream_unstable_v1_close(kde_screencast_stream_v1_);
        kde_screencast_stream_v1_ = nullptr;
      }
      if (kde_screencast_v1_) {
        zkde_screencast_unstable_v1_destroy(kde_screencast_v1_);
        kde_screencast_v1_ = nullptr;
      }
      if (kde_output_order) {
        kde_output_order_v1_destroy(kde_output_order);
        kde_output_order = nullptr;
      }

      // Clear output order list
      output_order.clear();
      // Clear current output parameters
      out_params.reset();
      out_params = nullptr;

      // wl_output is owned by the registry, released on disconnect
      // also cleanup associated output parameters and clear output list when done
      for (auto &[output, params] : outputs) {
        wl_output_destroy(output);
        params.reset();
      }
      outputs.clear();

      // Release wayland registry, display and reset pointers
      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }
      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin wayland, enumerate outputs.
     * @param setup_permissions - Try to setup KWin permissions (default: true)
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int init(const bool setup_permissions = true) {
      if (setup_permissions) {
        // Try to set up permissions for zkde_screencast_unstable_v1
        screencast_permission_helper_t::setup();
      }

      if (connect() < 0) {
        return -1;
      }

      if (!is_kwin_screencasting_available() && setup_permissions && screencast_permission_helper_t::fall_back_to_user_permission()) {
        // KWin decides which protocols to offer when a client connects, so reconnect after the
        // per-user permission file is in place; KDE may need a few more moments to index it.
        for (int attempt = 0; attempt < 5 && !is_kwin_screencasting_available(); ++attempt) {
          if (attempt > 0) {
            std::this_thread::sleep_for(1s);
          }
          disconnect();
          if (connect() < 0) {
            return -1;
          }
        }
      }

      if (!is_kwin_screencasting_available()) {
        BOOST_LOG(debug) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry."sv;
        return -1;
      }

      return 0;
    }

    /**
     * @brief Open the Wayland connection and bind KWin's globals.
     *
     * @return 0 on success, -1 on failure.
     */
    int connect() {
      std::string wl_name;
      if (!lizardbyte::common::get_env("WAYLAND_DISPLAY", wl_name)) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name.c_str());
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);

      // We need a second roundtrip after binding outputs to get wl_output events
      wl_display_roundtrip(wl_display);
      return 0;
    }

    /**
     * @brief Check if kwin screencasting is currently available
     *
     * @return true if screencast can be started, false otherwise
     */
    bool is_kwin_screencasting_available() const {
      return kde_screencast_v1_ != nullptr;
    }

    /**
     * @brief Generate a sorted list of known output names.
     * @return List of strings with output names to pass to start()
     */
    std::vector<std::string> get_output_names() {
      std::vector<std::shared_ptr<output_parameter_t>> sorted_outputs;
      for (const auto &output_parameter : outputs | std::views::values) {
        output_parameter->order = get_order_for_output_name(output_parameter->name);
        sorted_outputs.emplace_back(output_parameter);
      }
      std::ranges::sort(sorted_outputs, [](const auto &a, const auto &b) {
        return a->order < b->order || a->pos_x < b->pos_x || a->pos_y < b->pos_y;
      });
      std::vector<std::string> output_names;
      for (const auto &output_parameter : sorted_outputs) {
        BOOST_LOG(info) << "[kwingrab] Found output: "sv << output_parameter->name << " order: "sv << output_parameter->order << " position: "sv << output_parameter->pos_x << "x"sv << output_parameter->pos_y << " resolution: "sv << output_parameter->width << "x"sv << output_parameter->height;
        output_names.emplace_back(output_parameter->name);
      }
      return output_names;
    }

    /**
     * @brief Check if KWin is available for potential screencasting
     * @return True if KWin is detected
     */
    bool kwin_available() const {
      // Detect KWin using kde_output_order_v1 extension
      if (kde_output_order) {
        return true;
      }
      return false;
    }

    /**
     * @brief Request a screencast stream.
     * @param output_name Which wl_output to capture.
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int start(const std::string_view &output_name) {
      // Try find correct output by name
      if (outputs.empty()) {
        BOOST_LOG(error) << "[kwingrab] no wl_output found"sv;
        return -1;
      }
      struct wl_output *output = nullptr;
      if (!output_name.empty()) {
        for (auto const &[output_, params_] : outputs) {
          if (params_->name == output_name) {
            output = output_;
            out_params = params_;
          }
        }
      }
      // Fall back to first element from the map in case of error
      if (!output || !out_params) {
        const auto output_ = outputs.begin();
        output = output_->first;
        out_params = output_->second;
      }

      // Request a stream for the chosen output with embedded cursor
      if (kde_screencast_v1_) {
        kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_output(kde_screencast_v1_, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
        zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);
      } else {
        // No screencast protocol found. Output an error based on newly initialized permission file.
        if (screencast_permission_helper_t::is_newly_initialized()) {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. "sv
                              "A new permission desktop file was automatically created but might now have been recognized yet. "sv
                              "Try restarting sunshine or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        } else {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. Check permission desktop file "sv
                              "for sunshine binary or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        }
        return -1;
      }

      if (wait_for_stream() < 0) {
        return -1;
      }

      if (stream_failed) {
        BOOST_LOG(error) << "[kwingrab] stream_output failed: "sv << stream_error_msg;
        return -1;
      }
      // Check for valid node_id and/or object serial values here, stream_ready is just an internal flag
      if (out_node_id == PW_ID_ANY && (out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for created event"sv;
        return -1;
      }

      if ((out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: node="sv << out_node_id;
      } else {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: objectserial="sv << out_objectserial << " (node="sv << out_node_id << ")"sv;
      }

      if (out_params->width == 0 || out_params->height == 0) {
        BOOST_LOG(error) << "[kwingrab] could not determine output dimensions"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] Screencasting output"sv
                      << " name "sv << out_params->name
                      << " position "sv << out_params->pos_x << "x"sv << out_params->pos_y
                      << " resolution "sv << out_params->width << "x"sv << out_params->height;
      return 0;
    }

    /**
     * @brief Ask KWin to create a new virtual output and stream it.
     *
     * KWin keeps the output alive for as long as this stream (and connection) stays open and
     * removes it when the stream is closed. On success, `out_params` describes the new output
     * as seen by every Wayland client (name, position, mode).
     *
     * @param name Requested output name.
     * @param description User-visible description shown in display settings.
     * @param width Physical width in pixels.
     * @param height Physical height in pixels.
     * @param scale Compositor scale factor applied to the output.
     * @return 0 on success, -1 on failure.
     */
    int start_virtual(const std::string &name, const std::string &description, int width, int height, double scale) {
      if (!kde_screencast_v1_) {
        BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 unavailable; cannot create a virtual display"sv;
        return -1;
      }

      const auto version = zkde_screencast_unstable_v1_get_version(kde_screencast_v1_);
      if (version < ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_SINCE_VERSION) {
        BOOST_LOG(error) << "[kwingrab] KWin screencast protocol v"sv << version << " has no virtual output support"sv;
        return -1;
      }

      // The protocol takes the logical size; KWin multiplies it by the scale for the real mode.
      const int logical_width = std::max(1, static_cast<int>(std::lround(width / scale)));
      const int logical_height = std::max(1, static_cast<int>(std::lround(height / scale)));
      const auto fixed_scale = wl_fixed_from_double(scale);

      std::set<struct wl_output *> known_outputs;
      for (const auto &output : outputs | std::views::keys) {
        known_outputs.insert(output);
      }

      if (version >= ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_WITH_DESCRIPTION_SINCE_VERSION) {
        kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_virtual_output_with_description(
          kde_screencast_v1_, name.c_str(), description.c_str(), logical_width, logical_height, fixed_scale, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
        );
      } else {
        kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_virtual_output(
          kde_screencast_v1_, name.c_str(), logical_width, logical_height, fixed_scale, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
        );
      }
      zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);

      if (wait_for_stream() < 0) {
        return -1;
      }
      if (stream_failed || !stream_ready) {
        BOOST_LOG(error) << "[kwingrab] virtual output creation failed: "sv << (stream_error_msg.empty() ? "timeout"s : stream_error_msg);
        return -1;
      }

      // The new wl_output global shows up after the stream is created; bind it and wait for its
      // name and current mode so capture and input mapping can find it.
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (wl_display_roundtrip(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] lost Wayland connection while waiting for the virtual output"sv;
          return -1;
        }
        for (const auto &[output, params] : outputs) {
          if (known_outputs.contains(output) || params->name.empty() || params->width <= 0 || params->height <= 0) {
            continue;
          }
          out_params = params;
          BOOST_LOG(info) << "[kwingrab] Virtual output '"sv << params->name << "' created at "sv
                          << params->pos_x << "x"sv << params->pos_y << " with mode "sv
                          << params->width << "x"sv << params->height;
          return 0;
        }
        std::this_thread::sleep_for(20ms);
      }

      BOOST_LOG(error) << "[kwingrab] KWin created the virtual output stream but the output never appeared"sv;
      return -1;
    }

    /**
     * @brief Process Wayland events for this connection until @p stop_fd becomes readable.
     *
     * A long-lived connection must keep reading, otherwise KWin's queued events fill the socket
     * buffer and the compositor drops the client (and with it the virtual output).
     *
     * @param stop_fd Read end of a pipe; any data on it ends the loop.
     */
    void dispatch_until(int stop_fd) {
      const int display_fd = wl_display_get_fd(wl_display);
      while (true) {
        while (wl_display_prepare_read(wl_display) != 0) {
          wl_display_dispatch_pending(wl_display);
        }
        wl_display_flush(wl_display);

        std::array<struct pollfd, 2> fds {{
          {.fd = display_fd, .events = POLLIN, .revents = 0},
          {.fd = stop_fd, .events = POLLIN, .revents = 0},
        }};
        if (poll(fds.data(), fds.size(), -1) < 0) {
          wl_display_cancel_read(wl_display);
          if (errno == EINTR) {
            continue;
          }
          return;
        }
        if (fds[1].revents) {
          wl_display_cancel_read(wl_display);
          return;
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) {
          wl_display_cancel_read(wl_display);
          BOOST_LOG(warning) << "[kwingrab] virtual output connection closed by the compositor"sv;
          return;
        }
        if (wl_display_read_events(wl_display) < 0 || wl_display_dispatch_pending(wl_display) < 0) {
          BOOST_LOG(warning) << "[kwingrab] virtual output connection failed"sv;
          return;
        }
      }
    }

    uint32_t out_node_id = PW_ID_ANY;  ///< Out node ID.
    uint64_t out_objectserial = SPA_ID_INVALID;  ///< Out objectserial.
    std::shared_ptr<output_parameter_t> out_params = nullptr;  ///< Out params.

  private:
    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_order_v1 *kde_output_order = nullptr;
    struct zkde_screencast_unstable_v1 *kde_screencast_v1_ = nullptr;
    struct zkde_screencast_stream_unstable_v1 *kde_screencast_stream_v1_ = nullptr;
    std::map<struct wl_output *, std::shared_ptr<output_parameter_t>> outputs;
    std::vector<std::string> output_order;
    bool stream_failed = false;
    bool stream_ready = false;
    std::string stream_error_msg;

    // Misc functions
    int wait_for_stream() {
      // Dispatch until we get created/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stream_ready && !stream_failed && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed"sv;
          return -1;
        }
      }
      return 0;
    }

    size_t get_order_for_output_name(const std::string_view &name) const {
      for (size_t i = 0; i < output_order.size(); i++) {
        if (output_order[i] == name) {
          return i;
        }
      }
      // If nothing matches return list size (to ensure highest order)
      return output_order.size();
    }

    // Registry listener
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<screencast_t *>(data);
      if (!std::strcmp(interface, kde_output_order_v1_interface.name)) {
        // Bind version 1
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(1));
        self->kde_output_order = static_cast<struct kde_output_order_v1 *>(
          wl_registry_bind(reg, name, &kde_output_order_v1_interface, bind_ver)
        );
        kde_output_order_v1_add_listener(self->kde_output_order, &output_order_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_order_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
        // Bind version 1 to 6 — We use stream_output from v1 for node_id (deprecated but good as a fall-back)
        //                       but also try to get the newer (re-use safe) pipewire objectserial from v6
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(6));
        self->kde_screencast_v1_ = static_cast<struct zkde_screencast_unstable_v1 *>(
          wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver)
        );
        BOOST_LOG(debug) << "[kwingrab] bound zkde_screencast_unstable_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, wl_output_interface.name)) {
        // Bind version 4 - we need wl_output name for matching
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(4));
        auto *output = static_cast<struct wl_output *>(
          wl_registry_bind(reg, name, &wl_output_interface, bind_ver)
        );

        const auto [_, inserted] = self->outputs.try_emplace(output, std::make_shared<output_parameter_t>());
        if (inserted) {
          wl_output_add_listener(output, &output_listener, self);
          BOOST_LOG(debug) << "[kwingrab] bound wl_output version "sv << bind_ver << " instance: "sv << output;
        } else {
          // If we for some odd reason cannot add the output to the map clean it up and log a warning
          BOOST_LOG(warning) << "[kwingrab] Ignoring output "sv << output << " because map emplace failed."sv;
          wl_output_destroy(output);
        }
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {
      // We don't handle output hot-unplug during init
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    // wl_output listener (for mode/dimensions/name)
    static void on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw [[maybe_unused]], int32_t ph [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform [[maybe_unused]]) {
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->pos_x = x;
      output_parameter->pos_y = y;
    }

    static void on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh [[maybe_unused]]) {
      if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
      }
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->width = width;
      output_parameter->height = height;
    }

    static void on_output_done(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_scale(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], int32_t factor [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_name(void *data, struct wl_output *output, const char *name) {
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->name = name;
    }

    static void on_output_description(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], const char *description [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr struct wl_output_listener output_listener = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    // Output order listener
    static void on_output_order_output(void *data, struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]], const char *output_name) {
      auto *self = static_cast<screencast_t *>(data);
      self->output_order.emplace_back(output_name);
    }

    static void on_output_order_done(void *data [[maybe_unused]], struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr kde_output_order_v1_listener output_order_listener = {
      .output = on_output_order_output,
      .done = on_output_order_done,
    };

    // ScreenCast v1 stream listener
    static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]]) {
      auto *self = static_cast<screencast_t *>(data);
      BOOST_LOG(warning) << "[kwingrab] stream closed by server"sv;
      self->stream_failed = false;
      self->stream_ready = false;
      self->stream_error_msg = "stream closed by server";
    }

    static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const uint32_t node) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_node_id = node;
      self->stream_failed = false;
      self->stream_ready = true;
      BOOST_LOG(debug) << "[kwingrab] created event, node_id="sv << node;
    }

    static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const char *err_msg) {
      auto *self = static_cast<screencast_t *>(data);
      self->stream_failed = true;
      self->stream_ready = false;
      self->stream_error_msg = err_msg ? err_msg : "unknown error";
      BOOST_LOG(error) << "[kwingrab] failed event: "sv << self->stream_error_msg;
    }

    static void on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], uint32_t object_serial_hi, uint32_t object_serial_low) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_objectserial = static_cast<uint64_t>(object_serial_hi) << 32 | object_serial_low;
      // serial event always preceded the created event with the node id, so we only set stream_ready in created for v1
      BOOST_LOG(debug) << "[kwingrab] serial event, objectserial="sv << self->out_objectserial;
    }

    static constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };
  };

  /**
   * Display backend
   *
   * Orchestrates screencast_t and implements pipewire_display_t
   */
  class kwin_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0) {
        return -1;
      }
      if (screencast->start(display_name) < 0) {
        return -1;
      }
      if (screencast->out_params) {
        // Return values for pipewire init
        out_pipewire_fd = -1;  // KWin screencast capture runs on the local pipewire core
        out_pipewire_node = screencast->out_node_id;
        out_pipewire_objectserial = screencast->out_objectserial;
        // Set/update basic stream parameters on display_t
        this->offset_x = screencast->out_params->pos_x;
        this->offset_y = screencast->out_params->pos_y;
        this->width = screencast->out_params->width;
        this->height = screencast->out_params->height;
        this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
        this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
        return 0;
      }
      return -1;
    }

    std::unique_ptr<screencast_t> screencast;  ///< Screencast.
  };

  /**
   * KWin script that keeps streamed game windows on the virtual output.
   *
   * New windows open on whichever output KWin considers active (usually the one under the host's
   * mouse), so a game launched for the Moonlight player could otherwise appear on the host's own
   * monitor. While the virtual output exists, Steam Big Picture and Steam game windows are sent to it.
   */
  class window_router_t {
  public:
    window_router_t(const window_router_t &) = delete;
    window_router_t &operator=(const window_router_t &) = delete;

    explicit window_router_t(const std::string &output_name) {
      const auto runtime_dir = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
      if (runtime_dir.empty()) {
        BOOST_LOG(warning) << "[kwingrab] XDG_RUNTIME_DIR unset; game windows will not be moved to the virtual display"sv;
        return;
      }
      script_path = std::filesystem::path(runtime_dir) / (std::string(plugin_name) + ".js");

      std::ofstream script(script_path, std::ios::trunc);
      if (!script) {
        BOOST_LOG(warning) << "[kwingrab] cannot write KWin window-routing script to "sv << script_path;
        return;
      }
      script << script_source(output_name);
      script.close();

      GError *error = nullptr;
      bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (!bus) {
        BOOST_LOG(warning) << "[kwingrab] no session bus for KWin scripting: "sv << (error ? error->message : "unknown");
        g_clear_error(&error);
        return;
      }

      // Drop a stale copy from a previous run before loading the fresh one.
      call("unloadScript", g_variant_new("(s)", plugin_name), G_VARIANT_TYPE("(b)"));
      if (!call("loadScript", g_variant_new("(ss)", script_path.c_str(), plugin_name), G_VARIANT_TYPE("(i)"))) {
        return;
      }
      if (!call("start", nullptr, nullptr)) {
        return;
      }
      loaded = true;
      BOOST_LOG(info) << "[kwingrab] Routing Steam game windows to virtual output '"sv << output_name << '\'';
    }

    ~window_router_t() {
      if (loaded) {
        call("unloadScript", g_variant_new("(s)", plugin_name), G_VARIANT_TYPE("(b)"));
      }
      if (bus) {
        g_object_unref(bus);
      }
      if (!script_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(script_path, ec);
      }
    }

  private:
    static constexpr const char *plugin_name = "sunshine-virtual-display";

    GDBusConnection *bus = nullptr;
    std::filesystem::path script_path;
    bool loaded = false;

    bool call(const char *method, GVariant *parameters, const GVariantType *reply_type) {
      GError *error = nullptr;
      GVariant *reply = g_dbus_connection_call_sync(
        bus,
        "org.kde.KWin",
        "/Scripting",
        "org.kde.kwin.Scripting",
        method,
        parameters,
        reply_type,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        nullptr,
        &error
      );
      if (!reply) {
        BOOST_LOG(warning) << "[kwingrab] KWin scripting call "sv << method << " failed: "sv << (error ? error->message : "unknown");
        g_clear_error(&error);
        return false;
      }
      g_variant_unref(reply);
      return true;
    }

    static std::string script_source(const std::string &output_name) {
      // JSON-style escaping is enough for a JS string literal containing an output name.
      std::string quoted = "\"";
      for (const char c : output_name) {
        if (c == '"' || c == '\\') {
          quoted += '\\';
        }
        quoted += c;
      }
      quoted += '"';

      return "const targetName = " + quoted + R"JS(;

function targetOutput() {
  for (const output of workspace.screens) {
    if (output.name === targetName) {
      return output;
    }
  }
  return null;
}

function isStreamedGame(window) {
  if (!window || !window.normalWindow) {
    return false;
  }
  // X11 windows carry both an instance (resourceName) and a class (resourceClass); match either.
  const names = [window.resourceClass, window.resourceName].map((name) => String(name || "").toLowerCase());
  return names.some((name) => name.startsWith("steam_app_") || name === "gamescope") ||
    String(window.caption || "") === "Steam Big Picture Mode";
}

function route(window) {
  const output = targetOutput();
  if (output && isStreamedGame(window) && window.output !== output) {
    workspace.sendClientToScreen(window, output);
  }
}

function track(window) {
  route(window);
  window.captionChanged.connect(() => route(window));
}

workspace.windowAdded.connect(track);
for (const window of workspace.windowList()) {
  track(window);
}
)JS";
    }
  };

  /**
   * Session-scoped virtual output.
   *
   * Holds a dedicated Wayland connection with an open `stream_virtual_output` stream, which is what
   * keeps KWin's virtual output alive. Capture uses ordinary `stream_output` streams on this output,
   * so encoder probing and capture re-initialization never add or remove monitors.
   */
  class virtual_output_t {
  public:
    virtual_output_t(const virtual_output_t &) = delete;
    virtual_output_t &operator=(const virtual_output_t &) = delete;
    virtual_output_t() = default;

    ~virtual_output_t() {
      stop();
    }

    int start(const std::string &description, int width, int height, double scale, bool route_windows) {
      if (pipe(stop_pipe.data()) < 0) {
        BOOST_LOG(error) << "[kwingrab] cannot create virtual output control pipe"sv;
        return -1;
      }

      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0 || screencast->start_virtual("Moonlight", description, width, height, scale) < 0) {
        screencast.reset();
        return -1;
      }
      name = screencast->out_params->name;

      dispatcher = std::thread([this]() {
        screencast->dispatch_until(stop_pipe[0]);
      });

      if (route_windows) {
        router = std::make_unique<window_router_t>(name);
      }
      return 0;
    }

    void stop() {
      router.reset();
      if (dispatcher.joinable()) {
        const char byte = 0;
        std::ignore = write(stop_pipe[1], &byte, 1);
        dispatcher.join();
      }
      // Closing the stream and connection makes KWin remove the output.
      screencast.reset();
      for (auto &fd : stop_pipe) {
        if (fd >= 0) {
          close(fd);
          fd = -1;
        }
      }
    }

    const std::string &output_name() const {
      return name;
    }

  private:
    std::unique_ptr<screencast_t> screencast;
    std::unique_ptr<window_router_t> router;
    std::thread dispatcher;
    std::array<int, 2> stop_pipe {-1, -1};
    std::string name;
  };

  /**
   * @brief Process-wide virtual display state shared by session hooks and capture.
   */
  struct virtual_display_state_t {
    std::mutex mutex;  ///< Guards every member below.
    std::unique_ptr<virtual_output_t> output;  ///< Active virtual output, if any.
    int width = 0;  ///< Physical width the active output was created with.
    int height = 0;  ///< Physical height the active output was created with.
    double scale = 1.0;  ///< Scale the active output was created with.
  };

  virtual_display_state_t &virtual_display_state() {
    static virtual_display_state_t state;
    return state;
  }

  /**
   * @brief Name of the active virtual output, or an empty string.
   */
  std::string active_virtual_output_name() {
    auto &state = virtual_display_state();
    std::lock_guard lock {state.mutex};
    return state.output ? state.output->output_name() : std::string {};
  }
}  // namespace kwin

// Public API for misc.cpp
namespace platf {
  bool kwin_capture_selected();  // misc.cpp

  /**
   * @brief Create a KWin screencast display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return KWin/PipeWire display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[kwingrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    // While a session owns a virtual output, it is the only thing worth capturing.
    auto target_name = display_name;
    if (auto virtual_name = kwin::active_virtual_output_name(); !virtual_name.empty()) {
      target_name = std::move(virtual_name);
    }

    auto display = std::make_shared<kwin::kwin_t>();
    if (display->init(hwdevice_type, target_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate KWin screencast display names.
   *
   * @return KWin display names, or an empty list when KWin capture is unavailable.
   */
  std::vector<std::string> kwin_display_names() {
    if (auto virtual_name = kwin::active_virtual_output_name(); !virtual_name.empty()) {
      return {std::move(virtual_name)};
    }

    const auto screencast = std::make_unique<kwin::screencast_t>();
    if (screencast->init() < 0) {
      return {};
    }
    return screencast->get_output_names();
  }

  /**
   * @brief Create (or reuse) the per-session virtual output at the client's resolution.
   *
   * @param width Client width in pixels.
   * @param height Client height in pixels.
   * @param client_name Paired client name, shown in the display description.
   * @return True when a virtual output is active afterwards.
   */
  bool kwin_virtual_display_start(int width, int height, const std::string &client_name) {
    const auto &settings = config::video.virtual_display;
    auto &state = kwin::virtual_display_state();
    std::lock_guard lock {state.mutex};

    if (!kwin_capture_selected()) {
      BOOST_LOG(info) << "[kwingrab] Virtual display needs KWin capture (capture = kwin); streaming an existing monitor"sv;
      return false;
    }
    if (width <= 0 || height <= 0) {
      BOOST_LOG(warning) << "[kwingrab] client requested no resolution; not creating a virtual display"sv;
      return false;
    }
    if (state.output && state.width == width && state.height == height && state.scale == settings.scale) {
      BOOST_LOG(info) << "[kwingrab] Reusing virtual output '"sv << state.output->output_name() << '\'';
      return true;
    }

    state.output.reset();
    auto output = std::make_unique<kwin::virtual_output_t>();
    const auto description = client_name.empty() ? "Moonlight"s : "Moonlight (" + client_name + ")";
    if (output->start(description, width, height, settings.scale, settings.move_game_windows) < 0) {
      BOOST_LOG(warning) << "[kwingrab] Virtual display unavailable; streaming an existing monitor instead"sv;
      return false;
    }

    state.output = std::move(output);
    state.width = width;
    state.height = height;
    state.scale = settings.scale;
    return true;
  }

  /**
   * @brief Remove the per-session virtual output, if one exists.
   */
  void kwin_virtual_display_stop() {
    auto &state = kwin::virtual_display_state();
    std::lock_guard lock {state.mutex};
    if (!state.output) {
      return;
    }

    BOOST_LOG(info) << "[kwingrab] Removing virtual output '"sv << state.output->output_name() << '\'';
    state.output.reset();
    state.width = 0;
    state.height = 0;
  }

  /**
   * @brief Check whether KWin screencast capture is available.
   *
   * @return True when KWin capture support is available.
   */
  bool kwin_available() {
    // Init screencast without permission setup (to not cause unneeded logs / temporary desktop files) and check KWin availability
    if (const auto screencast = std::make_unique<kwin::screencast_t>(); screencast->init(false) < 0 || !screencast->kwin_available()) {
      return false;
    }
    return true;
  }

  /**
   * @brief Report whether a per-session virtual output currently exists.
   *
   * @return True while a session owns a virtual output.
   */
  bool kwin_virtual_display_active() {
    return !kwin::active_virtual_output_name().empty();
  }
}  // namespace platf
