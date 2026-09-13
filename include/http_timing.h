#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <utility>

// A small adapter for Arduino WebServer's existing virtual write hooks and
// middleware. This header itself needs neither Arduino nor its String class.
namespace http_timing {
constexpr uint32_t kSlowRequestUs = 50000;

enum class Route : uint8_t { Root, Track, Status, Debug, Log, Control, Other };
inline const char *routeName(Route route) {
  switch (route) {
    case Route::Root: return "root";
    case Route::Track: return "track";
    case Route::Status: return "status";
    case Route::Debug: return "debug";
    case Route::Log: return "log";
    case Route::Control: return "control";
    default: return "other";
  }
}
inline Route classifyRoute(const char *uri) {
  if (!uri) return Route::Other;
  if (strcmp(uri, "/") == 0) return Route::Root;
  if (strcmp(uri, "/api/track") == 0) return Route::Track;
  if (strcmp(uri, "/api/status") == 0) return Route::Status;
  if (strcmp(uri, "/api/debug") == 0) return Route::Debug;
  if (strcmp(uri, "/api/log") == 0) return Route::Log;
  if (strcmp(uri, "/api/servo") == 0 ||
      strncmp(uri, "/api/servo/", 11) == 0 ||
      strncmp(uri, "/api/track/", 11) == 0 ||
      strcmp(uri, "/api/whitelist") == 0) return Route::Control;
  return Route::Other;
}

struct Snapshot {
  Route route = Route::Other;
  uint32_t totalUs = 0, preHandlerUs = 0, buildUs = 0, writeUs = 0, otherUs = 0;
  uint32_t bytesWritten = 0, shortWrites = 0;
};

struct Stats {
  uint32_t requests = 0, pollsWithoutRequest = 0, slowRequests = 0;
  uint32_t maxTotalUs = 0, maxPreHandlerUs = 0, maxBuildUs = 0;
  uint32_t maxWriteUs = 0, maxOtherUs = 0, maxPollWithoutRequestUs = 0;
  Snapshot last, slowest;
};

// Base must provide Base(int), virtual handleClient(), uri().c_str(),
// addMiddleware(callable(Base&, next)->bool), and the two protected virtual
// _currentClientWrite[_P](const char*,size_t) hooks. On ESP32 PGM_P aliases
// const char*. Clock must provide static uint32_t nowUs().
//
// Per completed request:
//   totalUs == preHandlerUs + buildUs + writeUs + otherUs
// preHandlerUs is pre-middleware accept/parser/dispatch work, not pure parsing.
// Any intercepted pre-handler write is charged to writeUs instead, once.
// Explicit measureBuild() scopes and intercepted writes can nest: the innermost
// phase owns elapsed time. otherUs contains the remaining handler/post-handler
// work. Writes measure local calls and their actual returned byte count, not
// delivery acknowledgements or pure network time.
//
// A handleClient() call without entering middleware is NOT a request. Its whole
// duration contributes only to pollsWithoutRequest/maxPollWithoutRequestUs,
// including idle waits, rejected parses, and parser error responses.
//
// Snapshots update only after handleClient() finishes. A response reading
// timing() therefore sees previous completed requests. Clock subtraction is
// wrap-safe provided a single handleClient() lasts less than 2^32 microseconds.
template <class Base, class Clock>
class Server : public Base {
 public:
  explicit Server(int port = 80) : Base(port) {
    this->addMiddleware([this](Base &server, auto next) -> bool {
      if (inPoll_ && !sawHandler_) {
        sawHandler_ = true;
        current_.route = classifyRoute(server.uri().c_str());
        transition(Phase::Other);
      }
      return next();
    });
  }
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  void handleClient() override {
    if (inPoll_) { Base::handleClient(); return; }
    PollScope poll(*this);
    Base::handleClient();
  }

  template <class Build>
  auto measureBuild(Build &&build) -> decltype(std::forward<Build>(build)()) {
    PhaseScope scope(*this, Phase::Build, inPoll_ && sawHandler_);
    return std::forward<Build>(build)();
  }

  const Stats &timing() const { return stats_; }

 protected:
  size_t _currentClientWrite(const char *data, size_t length) override {
    return measuredWrite([&]() { return Base::_currentClientWrite(data, length); }, length);
  }
  size_t _currentClientWrite_P(const char *data, size_t length) override {
    return measuredWrite([&]() { return Base::_currentClientWrite_P(data, length); }, length);
  }

 private:
  enum class Phase : uint8_t { PreHandler, Build, Write, Other };

  static void addSaturating(uint32_t &value, size_t amount = 1) {
    value = amount > UINT32_MAX - value ? UINT32_MAX : value + static_cast<uint32_t>(amount);
  }
  static void updateMax(uint32_t &maximum, uint32_t value) {
    if (value > maximum) maximum = value;
  }

  void charge(uint32_t now) {
    const uint32_t elapsed = now - phaseStartedUs_;
    switch (phase_) {
      case Phase::PreHandler: current_.preHandlerUs += elapsed; break;
      case Phase::Build: current_.buildUs += elapsed; break;
      case Phase::Write: current_.writeUs += elapsed; break;
      case Phase::Other: current_.otherUs += elapsed; break;
    }
    phaseStartedUs_ = now;
  }
  void transition(Phase next) {
    charge(Clock::nowUs());
    phase_ = next;
  }
  void beginPoll() {
    current_ = Snapshot{};
    sawHandler_ = false;
    writeDepth_ = 0;
    phase_ = Phase::PreHandler;
    startedUs_ = phaseStartedUs_ = Clock::nowUs();
    inPoll_ = true;
  }
  void finishPoll() {
    const uint32_t now = Clock::nowUs();
    charge(now);
    current_.totalUs = now - startedUs_;
    inPoll_ = false;
    if (!sawHandler_) {
      addSaturating(stats_.pollsWithoutRequest);
      updateMax(stats_.maxPollWithoutRequestUs, current_.totalUs);
      return;
    }
    addSaturating(stats_.requests);
    if (current_.totalUs >= kSlowRequestUs) addSaturating(stats_.slowRequests);
    updateMax(stats_.maxTotalUs, current_.totalUs);
    updateMax(stats_.maxPreHandlerUs, current_.preHandlerUs);
    updateMax(stats_.maxBuildUs, current_.buildUs);
    updateMax(stats_.maxWriteUs, current_.writeUs);
    updateMax(stats_.maxOtherUs, current_.otherUs);
    stats_.last = current_;
    if (stats_.requests == 1 || current_.totalUs > stats_.slowest.totalUs)
      stats_.slowest = current_;
  }

  class PollScope {
   public:
    explicit PollScope(Server &server) : server_(server) { server_.beginPoll(); }
    ~PollScope() { server_.finishPoll(); }
   private:
    Server &server_;
  };
  class PhaseScope {
   public:
    PhaseScope(Server &server, Phase phase, bool enabled)
        : server_(server), previous_(server.phase_), enabled_(enabled) {
      if (enabled_) server_.transition(phase);
    }
    ~PhaseScope() { if (enabled_) server_.transition(previous_); }
   private:
    Server &server_;
    Phase previous_;
    bool enabled_;
  };

  template <class Write>
  size_t measuredWrite(Write &&write, size_t requested) {
    if (!inPoll_) return std::forward<Write>(write)();
    PhaseScope scope(*this, Phase::Write, true);
    const bool outermost = writeDepth_++ == 0;
    // A Base implementation may implement write_P in terms of virtual write.
    // Its nested call must not count the same bytes/short write twice.
    struct DepthGuard {
      unsigned &depth;
      ~DepthGuard() { --depth; }
    } depthGuard{writeDepth_};
    const size_t actual = std::forward<Write>(write)();
    if (outermost) {
      addSaturating(current_.bytesWritten, actual);
      if (actual < requested) addSaturating(current_.shortWrites);
    }
    return actual;
  }

  Stats stats_;
  Snapshot current_;
  uint32_t startedUs_ = 0, phaseStartedUs_ = 0;
  Phase phase_ = Phase::PreHandler;
  unsigned writeDepth_ = 0;
  bool inPoll_ = false, sawHandler_ = false;
};
}  // namespace http_timing
