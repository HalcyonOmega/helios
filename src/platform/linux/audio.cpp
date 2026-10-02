/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <atomic>
#include <bitset>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

// lib includes
#include <boost/regex.hpp>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/thread_safe.h"

namespace platf {
  using namespace std::literals;

#ifdef SUNSHINE_BUILD_KWIN
  bool kwin_virtual_display_active();  // kwingrab.cpp
#endif

  /**
   * @brief Position mapping.
   */
  constexpr pa_channel_position_t position_mapping[] {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  /**
   * @brief Convert a PulseAudio operation result to a log string.
   *
   * @param name Human-readable name to assign.
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @return Value converted to string.
   */
  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

  /**
   * @brief PulseAudio recording stream and channel metadata.
   */
  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_simple, pa_simple_free> mic;  ///< PulseAudio simple recording stream for microphone capture.

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_buf Sample buf.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_buf) override {
      auto sample_size = sample_buf.size();

      auto buf = sample_buf.data();
      int status;
      if (pa_simple_read(mic.get(), buf, sample_size * sizeof(float), &status)) {
        BOOST_LOG(error) << "pa_simple_read() failed: "sv << pa_strerror(status);

        return capture_e::error;
      }

      return capture_e::ok;
    }
  };

  /**
   * @brief Create a microphone capture stream for the requested layout.
   *
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @param sample_rate Audio sample rate in hertz.
   * @param frame_size Number of samples captured per audio frame.
   * @param source_name Source name.
   * @return Microphone capture object for the requested audio layout.
   */
  std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    auto mic = std::make_unique<mic_attr_t>();

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
    pa_channel_map pa_map;

    pa_map.channels = channels;
    std::for_each_n(pa_map.map, pa_map.channels, [mapping](auto &channel) mutable {
      channel = position_mapping[*mapping++];
    });

    pa_buffer_attr pa_attr = {
      .maxlength = uint32_t(-1),
      .tlength = uint32_t(-1),
      .prebuf = uint32_t(-1),
      .minreq = uint32_t(-1),
      .fragsize = uint32_t(frame_size * channels * sizeof(float))
    };

    int status;

    mic->mic.reset(
      pa_simple_new(nullptr, "sunshine", pa_stream_direction_t::PA_STREAM_RECORD, source_name.c_str(), "sunshine-record", &ss, &pa_map, &pa_attr, &status)
    );

    if (!mic->mic) {
      auto err_str = pa_strerror(status);
      BOOST_LOG(error) << "pa_simple_new() failed: "sv << err_str;
      return nullptr;
    }

    return mic;
  }

  namespace pa {
    template<bool B, class T>
    struct add_const_helper;

    /**
     * @brief Template helper that preserves constness for const inputs.
     */
    template<class T>
    struct add_const_helper<true, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const std::remove_pointer_t<T> *;
    };

    /**
     * @brief Template helper that leaves non-const inputs mutable.
     */
    template<class T>
    struct add_const_helper<false, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const T *;
    };

    /**
     * @brief PulseAudio callback info type with pointer constness normalized.
     */
    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    /**
     * @brief Release memory allocated by PulseAudio.
     *
     * @param p Pointer allocated by PulseAudio and released with `pa_xfree`.
     */
    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    /**
     * @brief Owning pointer for a PulseAudio context.
     */
    using ctx_t = util::safe_ptr<pa_context, pa_context_unref>;
    /**
     * @brief Owning pointer for a PulseAudio mainloop.
     */
    using loop_t = util::safe_ptr<pa_mainloop, pa_mainloop_free>;
    /**
     * @brief Owning pointer for a PulseAudio asynchronous operation.
     */
    using op_t = util::safe_ptr<pa_operation, pa_operation_unref>;
    /**
     * @brief Owning pointer for PulseAudio strings allocated with `pa_xmalloc`.
     */
    using string_t = util::safe_ptr<char, pa_free<char>>;

    /**
     * @brief Callback wrapper for PulseAudio introspection results without an end marker.
     */
    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    /**
     * @brief Handle PulseAudio sink-input introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    /**
     * @brief Callback wrapper for PulseAudio introspection results with an end marker.
     */
    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    /**
     * @brief Handle PulseAudio source introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param eol PulseAudio end-of-list marker.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }

      f(ctx, i, eol);
    }

    /**
     * @brief Forward a PulseAudio integer callback value into a Sunshine alarm.
     *
     * @param ctx PulseAudio context that emitted the callback.
     * @param i Integer value returned by the PulseAudio operation.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void cb_i(ctx_t::pointer ctx, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    /**
     * @brief Translate PulseAudio context state changes into server events.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void ctx_state_cb(ctx_t::pointer ctx, void *userdata) {
      auto &f = *(std::function<void(ctx_t::pointer)> *) userdata;

      f(ctx);
    }

    /**
     * @brief Record completion of a PulseAudio asynchronous operation.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param status Native status code returned by the platform API.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void success_cb(ctx_t::pointer ctx, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    /**
     * @brief Parent process id read from `/proc/<pid>/stat`, or 0.
     *
     * @param pid Process to inspect.
     * @return Parent pid, or 0 when unknown.
     */
    pid_t parent_pid(pid_t pid) {
      std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
      std::string content;
      std::getline(stat, content);
      // The command name (field 2) is parenthesised and may contain spaces; fields resume after it.
      const auto close = content.rfind(')');
      if (close == std::string::npos || close + 2 >= content.size()) {
        return 0;
      }
      std::istringstream rest(content.substr(close + 2));
      char state = 0;
      pid_t ppid = 0;
      rest >> state >> ppid;
      return ppid;
    }

    /**
     * @brief Whether Steam launched @p pid as part of a game (Steam sets SteamGameId for games).
     *
     * @param pid Process to inspect.
     * @return True when the process environment carries a non-zero SteamGameId.
     */
    bool is_steam_game_process(pid_t pid) {
      std::ifstream environ("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      std::string entry;
      while (std::getline(environ, entry, '\0')) {
        if (entry.starts_with("SteamGameId=")) {
          const auto value = entry.substr(sizeof("SteamGameId=") - 1);
          return !value.empty() && value != "0";
        }
      }
      return false;
    }

    /**
     * @brief Whether an audio client belongs to the streamed app.
     *
     * Steam games are recognized by their SteamGameId (they are children of the Steam client, not
     * of Sunshine); anything else counts when Sunshine itself started it.
     *
     * @param pid Process id of the audio client.
     * @return True when the client's audio should go to the stream.
     */
    bool belongs_to_streamed_app(pid_t pid) {
      if (pid <= 1) {
        return false;
      }
      if (is_steam_game_process(pid)) {
        return true;
      }
      const auto self = getpid();
      for (int depth = 0; pid > 1 && depth < 64; ++depth) {
        pid = parent_pid(pid);
        if (pid == self) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief PulseAudio server controller that creates and removes Sunshine sinks.
     */
    class server_t: public audio_control_t {
      enum ctx_event_e : int {
        ready,
        terminated,
        failed
      };

    public:
      loop_t loop;  ///< PulseAudio threaded mainloop instance.
      ctx_t ctx;  ///< PulseAudio threaded mainloop context.
      std::string requested_sink;  ///< Requested sink.

      /**
       * @brief Sink index that the streamed app's audio is routed to, or PA_INVALID_INDEX.
       *
       * Read from the PulseAudio mainloop thread (subscription callbacks) and written by the
       * session thread that starts and stops routing.
       */
      std::atomic<std::uint32_t> routing_target {PA_INVALID_INDEX};
      std::mutex routed_mutex;  ///< Guards routed_inputs.
      std::set<std::uint32_t> routed_inputs;  ///< Sink inputs moved to the stream sink this session.

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;  ///< PulseAudio module index for the stereo null sink.
        std::uint32_t surround51 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 5.1 null sink.
        std::uint32_t surround71 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 7.1 null sink.
      } index;  ///< PulseAudio module indexes for Sunshine-created null sinks.

      std::unique_ptr<safe::event_t<ctx_event_e>> events;  ///< Event queue receiving PulseAudio context state changes.
      std::unique_ptr<std::function<void(ctx_t::pointer)>> events_cb;  ///< Callback that translates PulseAudio context updates into events.

      std::jthread worker;  ///< Thread running the PulseAudio mainloop.

      /**
       * @brief Initialize PulseAudio mainloop, context, and Sunshine null sinks.
       *
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init() {
        events = std::make_unique<safe::event_t<ctx_event_e>>();
        loop.reset(pa_mainloop_new());
        ctx.reset(pa_context_new(pa_mainloop_get_api(loop.get()), "sunshine"));

        events_cb = std::make_unique<std::function<void(ctx_t::pointer)>>([this](ctx_t::pointer ctx) {
          switch (pa_context_get_state(ctx)) {
            case PA_CONTEXT_READY:
              events->raise(ready);
              break;
            case PA_CONTEXT_TERMINATED:
              BOOST_LOG(debug) << "PulseAudio context terminated"sv;
              events->raise(terminated);
              break;
            case PA_CONTEXT_FAILED:
              BOOST_LOG(debug) << "PulseAudio context failed"sv;
              events->raise(failed);
              break;
            case PA_CONTEXT_CONNECTING:
              BOOST_LOG(debug) << "Connecting to pulseaudio"sv;
            case PA_CONTEXT_UNCONNECTED:
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_SETTING_NAME:
              break;
          }
        });

        pa_context_set_state_callback(ctx.get(), ctx_state_cb, events_cb.get());

        auto status = pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr);
        if (status) {
          BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(status);
          return -1;
        }

        worker = std::jthread {
          [](loop_t::pointer loop) {
            int retval;
            platf::set_thread_name("audio::pulseaudio");
            auto status = pa_mainloop_run(loop, &retval);

            if (status < 0) {
              BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
              return;
            }
          },
          loop.get()
        };

        auto event = events->pop();
        if (event == failed) {
          return -1;
        }

        return 0;
      }

      /**
       * @brief Create a PulseAudio null sink for one channel layout.
       *
       * @param name Human-readable name to assign.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @return PulseAudio module index for the new sink, or PA_INVALID_INDEX on failure.
       */
      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_load_module(
            ctx.get(),
            "module-null-sink",
            to_string(name, channel_mapping, channels).c_str(),
            cb_i,
            alarm.get()
          ),
        };

        alarm->wait();
        return *alarm->status();
      }

      /**
       * @brief Unload a Sunshine-created PulseAudio null sink.
       *
       * @param i PulseAudio introspection info supplied to the callback.
       * @return 0 when the sink is absent or unloaded; nonzero on PulseAudio failure.
       */
      int unload_null(std::uint32_t i) {
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_unload_module(ctx.get(), i, success_cb, alarm.get())
        };

        alarm->wait();

        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        return 0;
      }

      /**
       * @brief Query host and virtual sink names available to Sunshine.
       *
       * @return Host and virtual sink names when the backend can report them.
       */
      std::optional<sink_t> sink_info() override {
        constexpr auto stereo = "sink-sunshine-stereo";
        constexpr auto surround51 = "sink-sunshine-surround51";
        constexpr auto surround71 = "sink-sunshine-surround71";

        auto alarm = safe::make_alarm<int>();

        sink_t sink;

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(ctx));

              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          // Ensure Sunshine won't create a sink that already exists.
          if (!std::strcmp(sink_info->name, stereo)) {
            index.stereo = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround51)) {
            index.surround51 = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround71)) {
            index.surround71 = sink_info->owner_module;

            ++nullcount;
          }
        };

        op_t op {pa_context_get_sink_info_list(ctx.get(), cb<pa_sink_info *>, &f)};

        if (!op) {
          BOOST_LOG(error) << "Couldn't create card info operation: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return std::nullopt;
        }

        alarm->wait();

        if (*alarm->status()) {
          return std::nullopt;
        }

        auto sink_name = get_default_sink_name();
        sink.host = sink_name;

        if (index.stereo == PA_INVALID_INDEX) {
          index.stereo = load_null(stereo, speaker::map_stereo.data(), static_cast<int>(speaker::map_stereo.size()));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51.data(), static_cast<int>(speaker::map_surround51.size()));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71.data(), static_cast<int>(speaker::map_surround71.size()));
          if (index.surround71 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-71: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (sink_name.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          sink.null = std::make_optional(sink_t::null_t {stereo, surround51, surround71});
        }

        return std::make_optional(std::move(sink));
      }

      /**
       * @brief Get default sink name.
       *
       * @return PulseAudio name of the current default sink, or an empty string.
       */
      std::string get_default_sink_name() {
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(ctx));
            alarm->ring(-1);
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        alarm->wait();
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      /**
       * @brief Get monitor name.
       *
       * @param sink_name Sink name.
       * @return PulseAudio monitor source name for the supplied sink, or an empty string.
       */
      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          monitor_name = sink_info->monitor_source_name;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        alarm->wait();
        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      /**
       * @brief Create a microphone capture stream for the requested layout.
       *
       * @param mapping Opus channel mapping table for the requested layout.
       * @param channels Number of audio channels in the stream.
       * @param sample_rate Audio sample rate in hertz.
       * @param frame_size Number of samples captured per audio frame.
       * @param continuous_audio Continuous audio.
       * @param host_audio_enabled Whether host playback should remain enabled during capture.
       * @return Microphone capture object for the requested audio layout.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
        // Sink choice priority:
        // 1. Config sink
        // 2. Last sink swapped to (Usually virtual in this case)
        // 3. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = config::audio.sink;
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        return ::platf::microphone(mapping, channels, sample_rate, frame_size, get_monitor_name(sink_name));
      }

      bool is_sink_available(const std::string &sink) override {
        BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
        return true;
      }

      /**
       * @brief Index of the sink named @p name.
       *
       * @param name PulseAudio sink name.
       * @return Sink index, or nullopt when the sink does not exist.
       */
      std::optional<std::uint32_t> sink_index(const std::string &name) {
        auto alarm = safe::make_alarm<int>();
        std::optional<std::uint32_t> found;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer, const pa_sink_info *info, int eol) {
          if (!info) {
            alarm->ring(eol ? 0 : -1);
            return;
          }
          found = info->index;
        };

        op_t op {pa_context_get_sink_info_by_name(ctx.get(), name.c_str(), cb<pa_sink_info *>, &f)};
        if (!op) {
          return std::nullopt;
        }
        alarm->wait();
        return found;
      }

      /**
       * @brief Process id recorded in a PulseAudio property list, or 0.
       *
       * Pulse clients set application.process.id; PipeWire also records the socket peer as
       * pipewire.sec.pid on client objects.
       */
      static pid_t pid_from(const pa_proplist *properties) {
        for (const char *key : {PA_PROP_APPLICATION_PROCESS_ID, "pipewire.sec.pid"}) {
          if (const char *value = pa_proplist_gets(properties, key)) {
            return static_cast<pid_t>(std::strtol(value, nullptr, 10));
          }
        }
        return 0;
      }

      /**
       * @brief Move sink input @p input to the stream sink when @p pid belongs to the streamed app.
       *
       * Runs on the PulseAudio mainloop thread.
       */
      void route_if_streamed_app(pa_context *context, std::uint32_t input, pid_t pid, const std::string &app) {
        const auto target = routing_target.load();
        if (target == PA_INVALID_INDEX || !belongs_to_streamed_app(pid)) {
          return;
        }

        if (auto *op = pa_context_move_sink_input_by_index(context, input, target, nullptr, nullptr)) {
          pa_operation_unref(op);
        }
        std::lock_guard lock {routed_mutex};
        if (routed_inputs.insert(input).second) {
          BOOST_LOG(info) << "Streaming audio from ["sv << (app.empty() ? "unknown"s : app) << "] (pid "sv << pid << ')';
        }
      }

      /**
       * @brief Sink input waiting for its client's process id (native PipeWire streams carry it only there).
       */
      struct pending_input_t {
        server_t *self;  ///< Owning server.
        std::uint32_t input;  ///< Sink input index.
        std::string app;  ///< Application name for logging.
      };

      static void on_client_info(pa_context *context, const pa_client_info *client, int eol, void *userdata) {
        auto *pending = static_cast<pending_input_t *>(userdata);
        if (eol || !client) {
          delete pending;
          return;
        }
        pending->self->route_if_streamed_app(context, pending->input, pid_from(client->proplist), pending->app);
      }

      static void on_sink_input_info(pa_context *context, const pa_sink_input_info *input, int eol, void *userdata) {
        if (eol || !input) {
          return;
        }
        auto *self = static_cast<server_t *>(userdata);
        const auto target = self->routing_target.load();
        if (target == PA_INVALID_INDEX || input->sink == target) {
          return;
        }

        const char *name = pa_proplist_gets(input->proplist, PA_PROP_APPLICATION_NAME);
        std::string app = name ? name : "";
        if (const auto pid = pid_from(input->proplist); pid > 0) {
          self->route_if_streamed_app(context, input->index, pid, app);
          return;
        }
        if (input->client == PA_INVALID_INDEX) {
          return;
        }

        auto *pending = new pending_input_t {self, input->index, std::move(app)};
        if (auto *op = pa_context_get_client_info(context, input->client, on_client_info, pending)) {
          pa_operation_unref(op);
        } else {
          delete pending;
        }
      }

      static void on_subscription_event(pa_context *context, pa_subscription_event_type_t type, std::uint32_t index, void *userdata) {
        auto *self = static_cast<server_t *>(userdata);
        if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
          return;
        }
        if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
          std::lock_guard lock {self->routed_mutex};
          self->routed_inputs.erase(index);
          return;
        }
        if (self->routing_target.load() == PA_INVALID_INDEX) {
          return;
        }
        if (auto *op = pa_context_get_sink_input_info(context, index, on_sink_input_info, self)) {
          pa_operation_unref(op);
        }
      }

      /**
       * @brief Update the sink value on the backend.
       *
       * @param sink Audio sink name to route or capture.
       * @return Status from updating sink.
       */
      int set_sink(const std::string &sink) override {
        auto alarm = safe::make_alarm<int>();

        BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
        op_t op {
          pa_context_set_default_sink(
            ctx.get(),
            sink.c_str(),
            success_cb,
            alarm.get()
          ),
        };

        if (!op) {
          BOOST_LOG(error) << "Couldn't create set default-sink operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        alarm->wait();
        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return -1;
        }

        requested_sink = sink;

        return 0;
      }

      int route_app_audio(const std::string &sink) override {
#ifdef SUNSHINE_BUILD_KWIN
        // Only worth it when the session has its own screen; a mirrored desktop streams all audio.
        if (!kwin_virtual_display_active() || !proc::proc.app_launches_processes()) {
          return -1;
        }
#else
        return -1;
#endif

        const auto target = sink_index(sink);
        if (!target) {
          return -1;
        }

        requested_sink = sink;
        routing_target = *target;
        BOOST_LOG(info) << "Streaming only the app's audio to ["sv << sink << "]; this PC keeps its default output"sv;

        // Every callback below runs on the PulseAudio mainloop thread, so moves never race the loop.
        pa_context_set_subscribe_callback(ctx.get(), on_subscription_event, this);
        if (auto *op = pa_context_subscribe(ctx.get(), PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr)) {
          pa_operation_unref(op);
        }
        // Catch streams the app opened before routing started.
        if (auto *op = pa_context_get_sink_input_info_list(ctx.get(), on_sink_input_info, this)) {
          pa_operation_unref(op);
        }
        return 0;
      }

      void stop_app_audio_routing(const std::string &host_sink) override {
        if (routing_target.exchange(PA_INVALID_INDEX) == PA_INVALID_INDEX) {
          return;
        }
        if (auto *op = pa_context_subscribe(ctx.get(), PA_SUBSCRIPTION_MASK_NULL, nullptr, nullptr)) {
          pa_operation_unref(op);
        }

        std::set<std::uint32_t> routed;
        {
          std::lock_guard lock {routed_mutex};
          routed.swap(routed_inputs);
        }
        if (!host_sink.empty()) {
          for (const auto input : routed) {
            // Streams that already ended are gone; moving them simply fails.
            auto alarm = safe::make_alarm<int>();
            op_t op {pa_context_move_sink_input_by_name(ctx.get(), input, host_sink.c_str(), success_cb, alarm.get())};
            if (op) {
              alarm->wait();
            }
          }
          BOOST_LOG(info) << "Returned "sv << routed.size() << " app audio stream(s) to ["sv << host_sink << ']';
        }
        requested_sink.clear();
      }

      ~server_t() override {
        stop_app_audio_routing({});
        unload_null(index.stereo);
        unload_null(index.surround51);
        unload_null(index.surround71);

        if (worker.joinable()) {
          pa_context_disconnect(ctx.get());

          KITTY_WHILE_LOOP(auto event = events->pop(), event != terminated && event != failed, {
            event = events->pop();
          })

          pa_mainloop_quit(loop.get(), 0);
          worker.join();
        }
      }
    };
  }  // namespace pa

  /**
   * @brief Create the platform audio controller.
   */
  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
