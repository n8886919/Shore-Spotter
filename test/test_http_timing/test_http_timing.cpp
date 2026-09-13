#include <unity.h>
#include <functional>
#include <stdexcept>
#include <string>
#include "http_timing.h"

struct FakeClock {
  static uint32_t now;
  static uint32_t nowUs() { return now; }
  static void advance(uint32_t us) { now += us; }
};
uint32_t FakeClock::now = 0;

// The fake mirrors the real virtual hooks and middleware Function signature.
// All elapsed time is injected; these tests perform no HTTP or hardware I/O.
class FakeBase {
 public:
  using Next = std::function<bool()>;
  using Middleware = std::function<bool(FakeBase &, Next)>;
  explicit FakeBase(int port) : port(port) {}
  virtual ~FakeBase() = default;
  FakeBase &addMiddleware(Middleware middleware) {
    middleware_ = middleware;
    return *this;
  }
  virtual void handleClient() {
    FakeClock::advance(preUs);
    if (beforeMiddleware) beforeMiddleware();
    if (!hasRequest) return;
    middleware_(*this, [this]() {
      if (handler) handler();
      return true;
    });
    FakeClock::advance(postUs);
  }
  const std::string &uri() const { return path; }
  size_t write(size_t requested) { return _currentClientWrite("", requested); }
  size_t writeP(size_t requested) { return _currentClientWrite_P("", requested); }

  int port;
  std::string path = "/";
  uint32_t preUs = 0, postUs = 0, writeUs = 0, writePUs = 0;
  size_t writeLimit = SIZE_MAX;
  bool hasRequest = true, pDelegatesToWrite = false, failWrite = false;
  std::function<void()> beforeMiddleware, handler;

 protected:
  virtual size_t _currentClientWrite(const char *, size_t requested) {
    FakeClock::advance(writeUs);
    if (failWrite) throw std::runtime_error("write failed");
    return requested < writeLimit ? requested : writeLimit;
  }
  virtual size_t _currentClientWrite_P(const char *data, size_t requested) {
    FakeClock::advance(writePUs);
    if (pDelegatesToWrite) return _currentClientWrite(data, requested);
    return requested < writeLimit ? requested : writeLimit;
  }

 private:
  Middleware middleware_;
};
using TimedServer = http_timing::Server<FakeBase, FakeClock>;

void setUp() { FakeClock::now = 0; }
void tearDown() {}

void assertPhases(const http_timing::Snapshot &snapshot, uint32_t pre,
                  uint32_t build, uint32_t write, uint32_t other) {
  TEST_ASSERT_EQUAL_UINT32(pre, snapshot.preHandlerUs);
  TEST_ASSERT_EQUAL_UINT32(build, snapshot.buildUs);
  TEST_ASSERT_EQUAL_UINT32(write, snapshot.writeUs);
  TEST_ASSERT_EQUAL_UINT32(other, snapshot.otherUs);
  TEST_ASSERT_EQUAL_UINT32(pre + build + write + other, snapshot.totalUs);
}

void test_request_measures_parse_build_multiple_writes_and_post_work() {
  TimedServer server(8080);
  TEST_ASSERT_EQUAL_INT(8080, server.port);
  server.path = "/api/debug";
  server.preUs = 1000;
  server.postUs = 20;
  server.writeUs = 200;
  server.writePUs = 300;
  server.handler = [&]() {
    FakeClock::advance(40);
    const std::string body = server.measureBuild([]() {
      FakeClock::advance(500);
      return std::string("response");
    });
    TEST_ASSERT_EQUAL_STRING("response", body.c_str());
    TEST_ASSERT_EQUAL_UINT32(12, server.write(12));
    TEST_ASSERT_EQUAL_UINT32(8, server.write(8));
    TEST_ASSERT_EQUAL_UINT32(30, server.writeP(30));
    FakeClock::advance(60);
  };
  server.handleClient();
  const auto &stats = server.timing();
  TEST_ASSERT_EQUAL_UINT32(1, stats.requests);
  TEST_ASSERT_EQUAL_UINT32(0, stats.pollsWithoutRequest);
  TEST_ASSERT_EQUAL_UINT32(0, stats.slowRequests);
  TEST_ASSERT_EQUAL_STRING("debug", http_timing::routeName(stats.last.route));
  assertPhases(stats.last, 1000, 500, 700, 120);
  TEST_ASSERT_EQUAL_UINT32(50, stats.last.bytesWritten);
  TEST_ASSERT_EQUAL_UINT32(0, stats.last.shortWrites);
  assertPhases(stats.slowest, 1000, 500, 700, 120);
}

void test_short_writes_record_actual_bytes_including_zero() {
  TimedServer server;
  server.writeUs = 10;
  server.writePUs = 20;
  server.handler = [&]() {
    server.writeLimit = 3;
    TEST_ASSERT_EQUAL_UINT32(3, server.write(10));
    TEST_ASSERT_EQUAL_UINT32(2, server.write(2));
    server.writeLimit = 0;
    TEST_ASSERT_EQUAL_UINT32(0, server.writeP(6));
    TEST_ASSERT_EQUAL_UINT32(0, server.write(0));
  };
  server.handleClient();
  assertPhases(server.timing().last, 0, 0, 50, 0);
  TEST_ASSERT_EQUAL_UINT32(5, server.timing().last.bytesWritten);
  TEST_ASSERT_EQUAL_UINT32(2, server.timing().last.shortWrites);
}

void test_write_p_delegating_to_virtual_write_counts_once() {
  TimedServer server;
  server.pDelegatesToWrite = true;
  server.writePUs = 25;
  server.writeUs = 75;
  server.writeLimit = 4;
  server.handler = [&]() {
    TEST_ASSERT_EQUAL_UINT32(4, server.writeP(10));
  };
  server.handleClient();
  assertPhases(server.timing().last, 0, 0, 100, 0);
  TEST_ASSERT_EQUAL_UINT32(4, server.timing().last.bytesWritten);
  TEST_ASSERT_EQUAL_UINT32(1, server.timing().last.shortWrites);
}

void test_nested_builds_and_writes_have_exclusive_phase_ownership() {
  TimedServer server;
  server.writeUs = 70;
  server.handler = [&]() {
    const int result = server.measureBuild([&]() {
      FakeClock::advance(10);
      const int nested = server.measureBuild([]() {
        FakeClock::advance(20);
        return 4;
      });
      server.write(5);
      FakeClock::advance(30);
      return nested + 1;
    });
    TEST_ASSERT_EQUAL_INT(5, result);
    FakeClock::advance(40);
  };
  server.handleClient();
  assertPhases(server.timing().last, 0, 60, 70, 40);
}

void test_pre_handler_write_is_excluded_from_pre_handler_time() {
  TimedServer server;
  server.preUs = 50;
  server.writeUs = 100;
  server.beforeMiddleware = [&]() {
    server.write(15);
    FakeClock::advance(20);
  };
  server.handler = []() { FakeClock::advance(10); };
  server.handleClient();
  assertPhases(server.timing().last, 70, 0, 100, 10);
  TEST_ASSERT_EQUAL_UINT32(15, server.timing().last.bytesWritten);
}

void test_micros_wrap_preserves_all_phase_durations() {
  FakeClock::now = UINT32_MAX - 50;
  TimedServer server;
  server.path = "/api/log";
  server.preUs = 100;
  server.writeUs = 300;
  server.postUs = 400;
  server.handler = [&]() {
    server.measureBuild([]() { FakeClock::advance(200); });
    server.write(1);
  };
  server.handleClient();
  assertPhases(server.timing().last, 100, 200, 300, 400);
  TEST_ASSERT_EQUAL_UINT32(1000, server.timing().maxTotalUs);
}

void test_idle_and_parse_failure_count_overhead_without_request() {
  TimedServer server;
  server.path = "/api/status";
  server.preUs = 100;
  server.handleClient();
  server.hasRequest = false;
  server.preUs = 60000;
  server.handleClient();
  server.preUs = 20;
  server.writeUs = 100000;
  server.beforeMiddleware = [&]() { server.write(12); };
  server.handleClient();
  const auto &stats = server.timing();
  TEST_ASSERT_EQUAL_UINT32(1, stats.requests);
  TEST_ASSERT_EQUAL_UINT32(2, stats.pollsWithoutRequest);
  TEST_ASSERT_EQUAL_UINT32(0, stats.slowRequests);
  TEST_ASSERT_EQUAL_UINT32(100020, stats.maxPollWithoutRequestUs);
  TEST_ASSERT_EQUAL_UINT32(100, stats.maxTotalUs);
  TEST_ASSERT_EQUAL_UINT32(0, stats.maxWriteUs);
  TEST_ASSERT_EQUAL_STRING("status", http_timing::routeName(stats.last.route));
  assertPhases(stats.last, 100, 0, 0, 0);
  assertPhases(stats.slowest, 100, 0, 0, 0);
}

void test_slow_threshold_phase_maxima_and_slowest_are_independent() {
  TimedServer server;
  server.path = "/api/track";
  server.preUs = 49999;
  server.handleClient();
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().slowRequests);
  server.path = "/api/servo/alpha";
  server.preUs = 0;
  server.handler = [&]() {
    server.measureBuild([]() { FakeClock::advance(50000); });
  };
  server.handleClient();
  TEST_ASSERT_EQUAL_UINT32(1, server.timing().slowRequests);
  server.path = "/";
  server.preUs = 10;
  server.postUs = 100;
  server.writePUs = 400;
  server.handler = [&]() { server.writeP(20); };
  server.handleClient();
  const auto &stats = server.timing();
  TEST_ASSERT_EQUAL_UINT32(3, stats.requests);
  TEST_ASSERT_EQUAL_UINT32(1, stats.slowRequests);
  TEST_ASSERT_EQUAL_UINT32(50000, stats.maxTotalUs);
  TEST_ASSERT_EQUAL_UINT32(49999, stats.maxPreHandlerUs);
  TEST_ASSERT_EQUAL_UINT32(50000, stats.maxBuildUs);
  TEST_ASSERT_EQUAL_UINT32(400, stats.maxWriteUs);
  TEST_ASSERT_EQUAL_UINT32(100, stats.maxOtherUs);
  TEST_ASSERT_EQUAL_STRING("root", http_timing::routeName(stats.last.route));
  TEST_ASSERT_EQUAL_STRING("control", http_timing::routeName(stats.slowest.route));
}

void test_snapshot_is_published_only_after_request_finishes() {
  TimedServer server;
  server.preUs = 25;
  server.handler = [&]() {
    TEST_ASSERT_EQUAL_UINT32(0, server.timing().requests);
  };
  server.handleClient();
  server.preUs = 40;
  server.handler = [&]() {
    TEST_ASSERT_EQUAL_UINT32(1, server.timing().requests);
    TEST_ASSERT_EQUAL_UINT32(25, server.timing().last.totalUs);
  };
  server.handleClient();
  TEST_ASSERT_EQUAL_UINT32(2, server.timing().requests);
  TEST_ASSERT_EQUAL_UINT32(40, server.timing().last.totalUs);
}

void test_build_and_write_outside_poll_do_not_create_request() {
  TimedServer server;
  server.writeUs = 20;
  int value = 7;
  int &result = server.measureBuild([&]() -> int & {
    FakeClock::advance(10);
    return value;
  });
  result = 8;
  TEST_ASSERT_EQUAL_INT(8, value);
  TEST_ASSERT_EQUAL_UINT32(30, server.write(30));
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().requests);
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().pollsWithoutRequest);
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().maxBuildUs);
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().maxWriteUs);
}

void test_build_error_finalizes_request_and_next_request_is_clean() {
  TimedServer server;
  server.preUs = 10;
  server.handler = [&]() {
    server.measureBuild([]() -> std::string {
      FakeClock::advance(20);
      throw std::runtime_error("builder failed");
    });
  };
  bool threw = false;
  try { server.handleClient(); } catch (const std::runtime_error &) { threw = true; }
  TEST_ASSERT_TRUE(threw);
  assertPhases(server.timing().last, 10, 20, 0, 0);
  server.handler = []() { FakeClock::advance(40); };
  server.handleClient();
  TEST_ASSERT_EQUAL_UINT32(2, server.timing().requests);
  assertPhases(server.timing().last, 10, 0, 0, 40);
}

void test_parse_error_and_write_error_restore_poll_and_write_state() {
  TimedServer server;
  server.preUs = 10;
  server.beforeMiddleware = []() { throw std::runtime_error("parse failed"); };
  bool threw = false;
  try { server.handleClient(); } catch (const std::runtime_error &) { threw = true; }
  TEST_ASSERT_TRUE(threw);
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().requests);
  TEST_ASSERT_EQUAL_UINT32(1, server.timing().pollsWithoutRequest);
  TEST_ASSERT_EQUAL_UINT32(10, server.timing().maxPollWithoutRequestUs);
  server.beforeMiddleware = nullptr;
  server.writeUs = 20;
  server.failWrite = true;
  server.handler = [&]() { server.write(8); };
  threw = false;
  try { server.handleClient(); } catch (const std::runtime_error &) { threw = true; }
  TEST_ASSERT_TRUE(threw);
  assertPhases(server.timing().last, 10, 0, 20, 0);
  TEST_ASSERT_EQUAL_UINT32(0, server.timing().last.bytesWritten);
  server.failWrite = false;
  server.handleClient();
  TEST_ASSERT_EQUAL_UINT32(2, server.timing().requests);
  TEST_ASSERT_EQUAL_UINT32(8, server.timing().last.bytesWritten);
  assertPhases(server.timing().last, 10, 0, 20, 0);
}

void test_route_classification_uses_exact_endpoints_and_control_prefixes() {
  struct Case { const char *path; const char *name; };
  const Case cases[] = {
    {"/", "root"}, {"/api/track", "track"}, {"/api/status", "status"},
    {"/api/debug", "debug"}, {"/api/log", "log"}, {"/api/servo", "control"},
    {"/api/servo/config", "control"}, {"/api/track/mode", "control"},
    {"/api/whitelist", "control"}, {"/api/servofoo", "other"},
    {"/favicon.ico", "other"}, {nullptr, "other"}
  };
  for (const auto &entry : cases)
    TEST_ASSERT_EQUAL_STRING(entry.name,
      http_timing::routeName(http_timing::classifyRoute(entry.path)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_request_measures_parse_build_multiple_writes_and_post_work);
  RUN_TEST(test_short_writes_record_actual_bytes_including_zero);
  RUN_TEST(test_write_p_delegating_to_virtual_write_counts_once);
  RUN_TEST(test_nested_builds_and_writes_have_exclusive_phase_ownership);
  RUN_TEST(test_pre_handler_write_is_excluded_from_pre_handler_time);
  RUN_TEST(test_micros_wrap_preserves_all_phase_durations);
  RUN_TEST(test_idle_and_parse_failure_count_overhead_without_request);
  RUN_TEST(test_slow_threshold_phase_maxima_and_slowest_are_independent);
  RUN_TEST(test_snapshot_is_published_only_after_request_finishes);
  RUN_TEST(test_build_and_write_outside_poll_do_not_create_request);
  RUN_TEST(test_build_error_finalizes_request_and_next_request_is_clean);
  RUN_TEST(test_parse_error_and_write_error_restore_poll_and_write_state);
  RUN_TEST(test_route_classification_uses_exact_endpoints_and_control_prefixes);
  return UNITY_END();
}
