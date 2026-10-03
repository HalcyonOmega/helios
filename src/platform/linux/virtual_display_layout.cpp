/**
 * @file src/platform/linux/virtual_display_layout.cpp
 * @brief Virtual-only KWin output placement, without changing the physical desktop.
 */
#include "virtual_display_layout.h"

#include "src/logging.h"
#include "src/utility.h"

#include <cmath>
#include <cstring>
#include <kde-output-device-v2.h>
#include <kde-output-management-v2.h>
#include <map>
#include <wayland-client.h>

namespace platf {
  namespace {
    /** @brief Output properties received from KWin, used only for reading host geometry. */
    struct device_t {
      std::string name;  ///< Wayland output name.
      int x = 0;  ///< Logical X position.
      int y = 0;  ///< Logical Y position.
      int transform = 0;  ///< Orientation reported by KWin.
      double scale = 1.0;  ///< Fractional compositor scale.
      bool enabled = false;  ///< Whether this output belongs to the desktop.
      kde_output_device_mode_v2 *current = nullptr;  ///< Current physical mode.
    };

    /** @brief Short-lived output-management connection; owns every bound proxy. */
    struct layout_t {
      wl_display *display = nullptr;  ///< Separate connection used for the single transaction.
      wl_registry *registry = nullptr;  ///< Registry proxy.
      kde_output_management_v2 *management = nullptr;  ///< Configuration factory.
      kde_output_device_registry_v2 *device_registry = nullptr;  ///< Modern output announcements.
      std::map<kde_output_device_v2 *, device_t> devices;  ///< Read-only output properties.
      std::map<kde_output_device_mode_v2 *, std::pair<int, int>> modes;  ///< Advertised pixel sizes.

      /** @brief Release client-side proxies and disconnect without modifying any output. */
      ~layout_t() {
        for (const auto &[mode, size] : modes) {
          kde_output_device_mode_v2_destroy(mode);
        }
        for (const auto &[device, properties] : devices) {
          kde_output_device_v2_destroy(device);
        }
        if (device_registry) {
          kde_output_device_registry_v2_destroy(device_registry);
        }
        if (management) {
          kde_output_management_v2_destroy(management);
        }
        if (registry) {
          wl_registry_destroy(registry);
        }
        if (display) {
          wl_display_disconnect(display);
        }
      }
    };

    /** @brief Callbacks for modes; no physical mode is ever requested by this client. */
    const kde_output_device_mode_v2_listener mode_listener {
      .size = [](void *data, kde_output_device_mode_v2 *mode, int32_t w, int32_t h) {
        static_cast<layout_t *>(data)->modes[mode] = {w, h};
      },
      .refresh = [](void *, kde_output_device_mode_v2 *, int32_t) {
      },
      .preferred = [](void *, kde_output_device_mode_v2 *) {
      },
      .removed = [](void *, kde_output_device_mode_v2 *) {
      },
      .flags = [](void *, kde_output_device_mode_v2 *, uint32_t) {
      },
    };

    /** @brief Read-only callbacks for both legacy globals and the version-21 device registry. */
    const kde_output_device_v2_listener device_listener {
      .geometry = [](void *data, kde_output_device_v2 *device, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char *, const char *, int32_t transform) {
        auto &properties = static_cast<layout_t *>(data)->devices.at(device);
        properties.x = x;
        properties.y = y;
        properties.transform = transform;
      },
      .current_mode = [](void *data, kde_output_device_v2 *device, kde_output_device_mode_v2 *mode) {
        static_cast<layout_t *>(data)->devices.at(device).current = mode;
      },
      .mode = [](void *data, kde_output_device_v2 *, kde_output_device_mode_v2 *mode) {
        static_cast<layout_t *>(data)->modes.emplace(mode, std::pair {0, 0});
        kde_output_device_mode_v2_add_listener(mode, &mode_listener, data);
      },
      .done = [](void *, kde_output_device_v2 *) {
      },
      .scale = [](void *data, kde_output_device_v2 *device, wl_fixed_t scale) {
        static_cast<layout_t *>(data)->devices.at(device).scale = wl_fixed_to_double(scale);
      },
      .edid = [](void *, kde_output_device_v2 *, const char *) {
      },
      .enabled = [](void *data, kde_output_device_v2 *device, int32_t enabled) {
        static_cast<layout_t *>(data)->devices.at(device).enabled = enabled != 0;
      },
      .uuid = [](void *, kde_output_device_v2 *, const char *) {
      },
      .serial_number = [](void *, kde_output_device_v2 *, const char *) {
      },
      .eisa_id = [](void *, kde_output_device_v2 *, const char *) {
      },
      .capabilities = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .overscan = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .vrr_policy = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .rgb_range = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .name = [](void *data, kde_output_device_v2 *device, const char *name) {
        static_cast<layout_t *>(data)->devices.at(device).name = name;
      },
      .high_dynamic_range = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .sdr_brightness = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .wide_color_gamut = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .auto_rotate_policy = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .icc_profile_path = [](void *, kde_output_device_v2 *, const char *) {
      },
      .brightness_metadata = [](void *, kde_output_device_v2 *, uint32_t, uint32_t, uint32_t) {
      },
      .brightness_overrides = [](void *, kde_output_device_v2 *, int32_t, int32_t, int32_t) {
      },
      .sdr_gamut_wideness = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .color_profile_source = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .brightness = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .color_power_tradeoff = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .dimming = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .replication_source = [](void *, kde_output_device_v2 *, const char *) {
      },
      .ddc_ci_allowed = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .max_bits_per_color = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .max_bits_per_color_range = [](void *, kde_output_device_v2 *, uint32_t, uint32_t) {
      },
      .automatic_max_bits_per_color_limit = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .edr_policy = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .sharpness = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .priority = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .auto_brightness = [](void *, kde_output_device_v2 *, uint32_t) {
      },
      .removed = [](void *data, kde_output_device_v2 *device) {
        static_cast<layout_t *>(data)->devices.at(device).enabled = false;
      },
    };

    /** @brief Accept output announcements on current Plasma without configuring their properties. */
    const kde_output_device_registry_v2_listener device_registry_listener {
      .finished = [](void *, kde_output_device_registry_v2 *) {
      },
      .output = [](void *data, kde_output_device_registry_v2 *, kde_output_device_v2 *device) {
        static_cast<layout_t *>(data)->devices.emplace(device, device_t {});
        kde_output_device_v2_add_listener(device, &device_listener, data);
      },
    };

    /** @brief Support legacy globals and modern registries, binding only supported callback versions. */
    const wl_registry_listener registry_listener {
      .global = [](void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
        auto &layout = *static_cast<layout_t *>(data);
        if (std::strcmp(interface, kde_output_management_v2_interface.name) == 0) {
          layout.management = static_cast<kde_output_management_v2 *>(wl_registry_bind(registry, id, &kde_output_management_v2_interface, 1));
        } else if (std::strcmp(interface, kde_output_device_v2_interface.name) == 0 && version >= 2) {
          auto *device = static_cast<kde_output_device_v2 *>(wl_registry_bind(registry, id, &kde_output_device_v2_interface, 2));
          layout.devices.emplace(device, device_t {});
          kde_output_device_v2_add_listener(device, &device_listener, data);
        } else if (std::strcmp(interface, kde_output_device_registry_v2_interface.name) == 0 && version >= 21) {
          layout.device_registry = static_cast<kde_output_device_registry_v2 *>(wl_registry_bind(registry, id, &kde_output_device_registry_v2_interface, 21));
          kde_output_device_registry_v2_add_listener(layout.device_registry, &device_registry_listener, data);
        }
      },
      .global_remove = [](void *, wl_registry *, uint32_t) {
      },
    };

    /** @brief Single-use transaction result reported by KWin. */
    struct result_t {
      bool done = false;  ///< Transaction completed.
      bool applied = false;  ///< Transaction accepted.
    };

    /** @brief Capture acknowledgment without changing any additional settings. */
    const kde_output_configuration_v2_listener configuration_listener {
      .applied = [](void *data, kde_output_configuration_v2 *) {
        *static_cast<result_t *>(data) = {true, true};
      },
      .failed = [](void *data, kde_output_configuration_v2 *) {
        *static_cast<result_t *>(data) = {true, false};
      },
    };
  }  // namespace

  bool position_virtual_output(const std::string &name) {
    // This helper must never be used to configure a physical output, even accidentally.
    if (!name.starts_with("Virtual-Helios-")) {
      return false;
    }
    layout_t layout;
    layout.display = wl_display_connect(nullptr);
    if (!layout.display) {
      return false;
    }
    layout.registry = wl_display_get_registry(layout.display);
    wl_registry_add_listener(layout.registry, &registry_listener, &layout);
    // Registry, output properties, then mode properties use separate event batches.
    for (int batch = 0; batch < 3; ++batch) {
      if (wl_display_roundtrip(layout.display) < 0) {
        return false;
      }
    }
    if (!layout.management) {
      return false;
    }
    kde_output_device_v2 *target = nullptr;
    std::vector<output_rect_t> host;
    for (const auto &[device, properties] : layout.devices) {
      if (properties.name == name) {
        target = device;
        continue;
      }
      const auto mode = layout.modes.find(properties.current);
      if (!properties.enabled || mode == layout.modes.end() || properties.scale <= 0) {
        continue;
      }
      auto [width, height] = mode->second;
      if (properties.transform % 2 != 0) {
        std::swap(width, height);
      }
      host.push_back({properties.x, properties.y, static_cast<int>(std::ceil(width / properties.scale)), static_cast<int>(std::ceil(height / properties.scale))});
    }
    const auto position = virtual_output_position(host);
    if (!target || !position) {
      return false;
    }
    auto *configuration = kde_output_management_v2_create_configuration(layout.management);
    auto cleanup = util::fail_guard([configuration]() {
      kde_output_configuration_v2_destroy(configuration);
    });
    result_t result;
    kde_output_configuration_v2_add_listener(configuration, &configuration_listener, &result);
    // The ONLY write in the transaction is this virtual output's position. Do not send
    // modes, scale, enable, primary or priority requests for any output.
    kde_output_configuration_v2_position(configuration, target, position->first, position->second);
    kde_output_configuration_v2_apply(configuration);
    if (wl_display_roundtrip(layout.display) < 0 || !result.done || !result.applied) {
      return false;
    }
    BOOST_LOG(info) << "[kwingrab] Isolated virtual output '" << name << "' at " << position->first << "x" << position->second;
    return true;
  }
}  // namespace platf
