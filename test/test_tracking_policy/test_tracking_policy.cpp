#include <unity.h>
#include "tracking_policy.h"
#include "loop_metrics.h"
using tracking_policy::Selector;
using tracking_policy::Source;
using tracking_policy::Mode;
void setUp() {}
void tearDown() {}

void test_gps_accepts_good_ok_and_rejects_bad_missing_stale() {
  TEST_ASSERT_TRUE(tracking_policy::usableGps(true, 8, 1.5f, 1999));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(true, 8, 1.5f, 2000));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(false, 8, 1.0f, 0));
  TEST_ASSERT_TRUE(tracking_policy::usableGps(true, 7, 1.0f, 0));
  TEST_ASSERT_TRUE(tracking_policy::usableGps(true, 8, 1.6f, 0));
  TEST_ASSERT_TRUE(tracking_policy::usableGps(true,6,3.0f,0));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(true,6,3.01f,0));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(true,5,1.0f,0));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(false,0,0,0));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(true, 8, -1.0f, 0));
  TEST_ASSERT_FALSE(tracking_policy::usableGps(true, 8, NAN, 0));
}

void test_gps_mode_never_falls_back_to_uart() {
  Selector s;
  TEST_ASSERT_TRUE(s.update(0, Mode::Gps, true, true) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(1999, Mode::Gps, true, true) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(2000, Mode::Gps, true, true) == Source::Gps);
  TEST_ASSERT_TRUE(s.update(2001, Mode::Gps, false, true) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(2002, Mode::Gps, false, false) == Source::Hold);
}

void test_uart_ignores_gps_and_manual_blocks_both() {
  Selector s;
  TEST_ASSERT_TRUE(s.update(0, Mode::Uart, true, true) == Source::Uart);
  TEST_ASSERT_TRUE(s.update(3000, Mode::Uart, true, true) == Source::Uart);
  TEST_ASSERT_FALSE(s.gpsReady());
  TEST_ASSERT_TRUE(s.update(3001, Mode::Uart, true, false) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(3002, Mode::Uart, false, true) == Source::Uart);
  TEST_ASSERT_TRUE(s.update(3003, Mode::Manual, true, true) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(3004, Mode::Paused, true, true) == Source::Hold);
}

void test_gps_flapping_and_mode_switch_reset_recovery() {
  Selector s;
  s.update(0, Mode::Gps, true, false);
  s.update(1999, Mode::Gps, false, false);
  TEST_ASSERT_TRUE(s.update(2000, Mode::Gps, true, false) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(4000, Mode::Gps, true, false) == Source::Gps);
  s.update(4001, Mode::Uart, true, true);
  TEST_ASSERT_TRUE(s.update(4002, Mode::Gps, true, true) == Source::Hold);
  TEST_ASSERT_TRUE(s.update(6002, Mode::Gps, true, true) == Source::Gps);
}

void test_recovery_and_deadline_rollover() {
  Selector s;
  const uint32_t start = UINT32_MAX - 1000;
  s.update(start, Mode::Gps, true, false);
  TEST_ASSERT_TRUE(s.update(start + 2000, Mode::Gps, true, false) == Source::Gps);
  s.reset();
  TEST_ASSERT_TRUE(s.update(start + 2001, Mode::Gps, true, false) == Source::Hold);
  TEST_ASSERT_FALSE(loop_metrics::due(start, start + 2000));
  TEST_ASSERT_TRUE(loop_metrics::due(start + 2000, start + 2000));
  TEST_ASSERT_TRUE(loop_metrics::due(start + 2001, start + 2000));
}

void test_duration_and_gap_record_actual_delays() {
  loop_metrics::Duration d;
  d.record(100, 200); d.record(1000, 52000); d.record(10, 30);
  TEST_ASSERT_EQUAL_UINT32(20, d.lastUs);
  TEST_ASSERT_EQUAL_UINT32(51000, d.maxUs);
  TEST_ASSERT_EQUAL_UINT32(1, d.over50ms);
  d.record(UINT32_MAX - 999, 50000);
  TEST_ASSERT_EQUAL_UINT32(51000, d.lastUs);
  loop_metrics::Gap g;
  const uint32_t start = UINT32_MAX - 100;
  g.observe(start);  // first observation must not count boot time
  TEST_ASSERT_EQUAL_UINT32(0, g.maxMs);
  g.observe(start + 10); g.observe(start + 310); g.observe(start + 320);
  TEST_ASSERT_EQUAL_UINT32(300, g.maxMs);
  TEST_ASSERT_EQUAL_UINT32(1, g.over250ms);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_gps_accepts_good_ok_and_rejects_bad_missing_stale);
  RUN_TEST(test_gps_mode_never_falls_back_to_uart);
  RUN_TEST(test_uart_ignores_gps_and_manual_blocks_both);
  RUN_TEST(test_gps_flapping_and_mode_switch_reset_recovery);
  RUN_TEST(test_recovery_and_deadline_rollover);
  RUN_TEST(test_duration_and_gap_record_actual_delays);
  return UNITY_END();
}
