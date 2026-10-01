#include <unity.h>
#include "client_cadence.h"
using client_cadence::Scheduler;
void setUp() {}
void tearDown() {}
void test_pair_and_no_duplicate_or_heartbeat() {
  Scheduler c; c.reset(100);
  TEST_ASSERT_FALSE(c.due(100, true, 0, false));
  TEST_ASSERT_FALSE(c.due(249, true, 0, false));
  TEST_ASSERT_TRUE(c.due(250, true, 0, false)); c.attempted(250, true, 0);
  TEST_ASSERT_FALSE(c.due(20000, true, 0, true));
  TEST_ASSERT_TRUE(c.due(20001, true, 500, true)); c.attempted(20001, true, 500);
  TEST_ASSERT_FALSE(c.due(40000, true, 500, true));
}
void test_two_hz_and_latest_only() {
  Scheduler c; c.reset(0);
  TEST_ASSERT_TRUE(c.due(0, true, 0, true)); c.attempted(0, true, 0);
  TEST_ASSERT_TRUE(c.due(500, true, 500, true)); // radio busy: do not consume
  TEST_ASSERT_TRUE(c.due(1000, true, 1000, true)); c.attempted(1000, true, 1000);
  TEST_ASSERT_FALSE(c.due(1001, true, 1000, true));
}
void test_invalid_transition_once_and_recovery_is_position() {
  Scheduler c; c.reset(100);
  TEST_ASSERT_FALSE(c.due(5000, false, 0, false));
  TEST_ASSERT_TRUE(c.due(5100, true, 100, true)); c.attempted(5100, true, 100);
  TEST_ASSERT_TRUE(c.due(5600, false, 600, true)); c.attempted(5600, false, 600);
  TEST_ASSERT_FALSE(c.due(60000, false, 600, true));
  TEST_ASSERT_TRUE(c.due(61000, true, 1600, true)); c.attempted(61000, true, 1600);
  TEST_ASSERT_FALSE(c.due(61500, true, 1600, true));
}
void test_pair_completion_midnight_and_millis_wrap() {
  Scheduler c; const uint32_t t=UINT32_MAX-50; c.reset(t);
  TEST_ASSERT_FALSE(c.due(t, true, 86399500, false));
  TEST_ASSERT_TRUE(c.due(t+90, true, 86399500, true)); c.attempted(t+90, true, 86399500);
  TEST_ASSERT_TRUE(c.due(t+500, true, 0, true));
}
void test_failed_local_attempt_not_retried() {
  Scheduler c; c.reset(0);
  TEST_ASSERT_TRUE(c.due(0, true, 0, true)); c.attempted(0, true, 0);
  TEST_ASSERT_FALSE(c.due(10000, true, 0, true));
  TEST_ASSERT_TRUE(c.due(10001, true, 500, true));
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pair_and_no_duplicate_or_heartbeat);
  RUN_TEST(test_two_hz_and_latest_only);
  RUN_TEST(test_invalid_transition_once_and_recovery_is_position);
  RUN_TEST(test_pair_completion_midnight_and_millis_wrap);
  RUN_TEST(test_failed_local_attempt_not_retried);
  return UNITY_END();
}
