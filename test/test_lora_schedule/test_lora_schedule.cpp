#include <unity.h>
#include "lora_schedule.h"
#include "packet_diagnostics.h"

void setUp() {}
void tearDown() {}

void test_claim_keeps_phase_and_skips_missed_slots_without_replay() {
  uint32_t next = 1000, skipped = 0;
  TEST_ASSERT_FALSE(lora_schedule::claim(999, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(1000, next);
  TEST_ASSERT_TRUE(lora_schedule::claim(1000, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(1500, next);
  TEST_ASSERT_EQUAL_UINT32(0, skipped);
  TEST_ASSERT_FALSE(lora_schedule::claim(1000, 500, next, skipped));
  TEST_ASSERT_TRUE(lora_schedule::claim(2499, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(2500, next);
  TEST_ASSERT_EQUAL_UINT32(1, skipped);
  TEST_ASSERT_FALSE(lora_schedule::claim(2499, 500, next, skipped));
  TEST_ASSERT_TRUE(lora_schedule::claim(5000, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(5500, next);
  TEST_ASSERT_EQUAL_UINT32(6, skipped);
  TEST_ASSERT_FALSE(lora_schedule::claim(5000, 500, next, skipped));
}

void test_claim_handles_millis_wrap_and_zero_period() {
  uint32_t next = UINT32_MAX - 200, skipped = 0;
  TEST_ASSERT_FALSE(lora_schedule::claim(next - 1, 500, next, skipped));
  const uint32_t start = next;
  TEST_ASSERT_TRUE(lora_schedule::claim(start, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(start + 500, next);
  TEST_ASSERT_TRUE(lora_schedule::claim(start + 1700, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(start + 2000, next);
  TEST_ASSERT_EQUAL_UINT32(2, skipped);
  const uint32_t unchanged = next;
  TEST_ASSERT_FALSE(lora_schedule::claim(start + 2500, 0, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(unchanged, next);
}

void test_late_data_is_skipped_before_transmit_can_collide() {
  uint32_t next = 1000, skipped = 0;
  TEST_ASSERT_TRUE(lora_schedule::claim(1300, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(1500, next);
  // Late DATA must not collide with the next slot, at either SF9 or SF10.
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1300, next, 330, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1300, next, 165, 80));
  TEST_ASSERT_TRUE(lora_schedule::claim(1500, 500, next, skipped));
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1500, next, 330, 80));
}

void test_data_fit_checks_sf10_sf9_and_precise_remaining_guard() {
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1090, 1500, 330, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1091, 1500, 330, 80));
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1255, 1500, 165, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1256, 1500, 165, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1500, 1500, 165, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1501, 1500, 165, 80));
  const uint32_t start = UINT32_MAX - 100;
  TEST_ASSERT_TRUE(lora_schedule::dataFits(start + 90, start + 500, 330, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(start + 91, start + 500, 330, 80));
}

void test_telemetry_respects_both_guards_and_exact_window_boundaries() {
  const uint32_t start = 1000, next = 1500;
  // Rounded-up actual SF9/125/4-5 times: DATA17=165, TEL11=145 ms.
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1244, start, next, 165, 145, 80, true));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1245, start, next, 165, 145, 80, true));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1275, start, next, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1276, start, next, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1500, start, next, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1501, start, next, 165, 145, 80, true));
}

void test_sf10_reserves_extra_slots_and_unfinished_data_blocks_extras() {
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 145, 80, true));
  TEST_ASSERT_TRUE(lora_schedule::dedicatedTelemetrySlot(500, 330, 289, 80));
  TEST_ASSERT_FALSE(lora_schedule::dedicatedTelemetrySlot(500, 165, 145, 80));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1000, 1500, 660, 80));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 145, 80, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 300, 80, true));
}

void test_telemetry_window_survives_wrap_and_late_data_start() {
  const uint32_t start = UINT32_MAX - 100;
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(start + 244, start, start + 500,
                                                165, 145, 80, true));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(start + 245, start, start + 500,
                                               165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(start + 276, start, start + 500,
                                                165, 145, 80, true));
  // DATA was sent 50 ms late: no room for telemetry and both guards remains.
  for (uint32_t now = 1050; now <= 1500; ++now)
    TEST_ASSERT_FALSE(lora_schedule::telemetryFits(now, 1050, 1500,
                                                 165, 145, 80, true));
}

void test_ring_retains_chronological_last_events_and_overwrite_count() {
  packet_diagnostics::Ring<3> ring;
  TEST_ASSERT_EQUAL_UINT32(0, ring.size());
  TEST_ASSERT_EQUAL_UINT32(0, ring.total());
  for (unsigned n = 0; n < 8; ++n) {
    packet_diagnostics::Event event;
    event.ms = 1000 + n * 500;
    event.seq = n;
    event.id = 100;  // ring assigns its own monotonic export ID
    event.kind = n % 2 ? packet_diagnostics::Kind::Telemetry : packet_diagnostics::Kind::Data;
    event.raw[0] = n;
    event.rawLength = 1;
    ring.push(event);
    event.raw[0] = 200;  // ring must own a copy
  }
  TEST_ASSERT_EQUAL_UINT32(3, ring.size());
  TEST_ASSERT_EQUAL_UINT32(8, ring.total());
  TEST_ASSERT_EQUAL_UINT32(5, ring.overwritten());
  for (unsigned i = 0; i < 3; ++i) {
    const auto &e = ring.at(i);
    TEST_ASSERT_EQUAL_UINT32(i + 6, e.id);
    TEST_ASSERT_EQUAL_UINT16(i + 5, e.seq);
    TEST_ASSERT_EQUAL_UINT32(3500 + i * 500, e.ms);
    TEST_ASSERT_EQUAL_UINT8(i + 5, e.raw[0]);
    TEST_ASSERT_EQUAL_UINT8(1, e.rawLength);
  }
}

void test_ring_exports_in_insertion_order_across_millis_wrap() {
  packet_diagnostics::Ring<2> ring;
  packet_diagnostics::Event e;
  e.ms = UINT32_MAX - 5;
  e.kind = packet_diagnostics::Kind::AckSkipped;
  ring.push(e);
  e.ms = 10;
  e.kind = packet_diagnostics::Kind::RadioError;
  ring.push(e);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 5, ring.at(0).ms);
  TEST_ASSERT_EQUAL_UINT32(10, ring.at(1).ms);
  TEST_ASSERT_EQUAL_STRING("ack_skipped", packet_diagnostics::name(ring.at(0).kind));
  TEST_ASSERT_EQUAL_STRING("radio_error", packet_diagnostics::name(ring.at(1).kind));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_claim_keeps_phase_and_skips_missed_slots_without_replay);
  RUN_TEST(test_claim_handles_millis_wrap_and_zero_period);
  RUN_TEST(test_late_data_is_skipped_before_transmit_can_collide);
  RUN_TEST(test_data_fit_checks_sf10_sf9_and_precise_remaining_guard);
  RUN_TEST(test_telemetry_respects_both_guards_and_exact_window_boundaries);
  RUN_TEST(test_sf10_reserves_extra_slots_and_unfinished_data_blocks_extras);
  RUN_TEST(test_telemetry_window_survives_wrap_and_late_data_start);
  RUN_TEST(test_ring_retains_chronological_last_events_and_overwrite_count);
  RUN_TEST(test_ring_exports_in_insertion_order_across_millis_wrap);
  return UNITY_END();
}
