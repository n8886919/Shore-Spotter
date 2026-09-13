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

void test_late_ack_cycle_is_skipped_before_transmit_can_collide() {
  uint32_t next = 1000, skipped = 0;
  TEST_ASSERT_TRUE(lora_schedule::claim(1300, 500, next, skipped));
  TEST_ASSERT_EQUAL_UINT32(1500, next);
  // Starting here would finish DATA at 1465 and ACK around 1610, colliding
  // with the next 1500 ms DATA. Claiming a slot does not authorize that TX.
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1300, next, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1300, next, 165, 145, 80, false));
  TEST_ASSERT_TRUE(lora_schedule::claim(1500, 500, next, skipped));
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1500, next, 165, 145, 80, true));
}

void test_data_fit_accounts_for_ack_and_precise_remaining_guard() {
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1110, 1500, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1111, 1500, 165, 145, 80, true));
  TEST_ASSERT_TRUE(lora_schedule::dataFits(1255, 1500, 165, 145, 80, false));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1256, 1500, 165, 145, 80, false));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1500, 1500, 165, 145, 80, false));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(1501, 1500, 165, 145, 80, false));
  const uint32_t start = UINT32_MAX - 100;
  TEST_ASSERT_TRUE(lora_schedule::dataFits(start + 110, start + 500, 165, 145, 80, true));
  TEST_ASSERT_FALSE(lora_schedule::dataFits(start + 111, start + 500, 165, 145, 80, true));
}

void test_telemetry_respects_both_guards_and_exact_window_boundaries() {
  const uint32_t start = 1000, next = 1500;
  // Rounded-up actual SF9/125/4-5 times: DATA17=165, TEL11=145 ms.
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1244, start, next, 165, 145, 80, true, false));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1245, start, next, 165, 145, 80, true, false));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1275, start, next, 165, 145, 80, true, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1276, start, next, 165, 145, 80, true, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1500, start, next, 165, 145, 80, true, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1501, start, next, 165, 145, 80, true, false));
}

void test_telemetry_never_runs_on_ack_or_unfinished_data_cycle() {
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 145, 80, true, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 145, 80, true, true));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 145, 80, false, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(1250, 1000, 1500, 165, 300, 80, true, false));
}

void test_telemetry_window_survives_wrap_and_late_data_start() {
  const uint32_t start = UINT32_MAX - 100;
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(start + 244, start, start + 500,
                                                165, 145, 80, true, false));
  TEST_ASSERT_TRUE(lora_schedule::telemetryFits(start + 245, start, start + 500,
                                               165, 145, 80, true, false));
  TEST_ASSERT_FALSE(lora_schedule::telemetryFits(start + 276, start, start + 500,
                                                165, 145, 80, true, false));
  // DATA was sent 50 ms late: no room for telemetry and both guards remains.
  for (uint32_t now = 1050; now <= 1500; ++now)
    TEST_ASSERT_FALSE(lora_schedule::telemetryFits(now, 1050, 1500,
                                                 165, 145, 80, true, false));
}

void test_ack_accepts_only_current_expected_seq_once() {
  lora_schedule::AckWindow ack;
  TEST_ASSERT_FALSE(ack.accept(8, 1000));
  ack.expect(8, 1000, 390);
  TEST_ASSERT_FALSE(ack.accept(0, 1100));
  TEST_ASSERT_FALSE(ack.accept(9, 1100));
  TEST_ASSERT_TRUE(ack.accept(8, 1300));
  TEST_ASSERT_FALSE(ack.accept(8, 1301));
}

void test_ack_expiry_replacement_and_clear() {
  lora_schedule::AckWindow ack;
  ack.expect(8, 1000, 390);
  TEST_ASSERT_FALSE(ack.accept(8, 1390));
  ack.expect(16, 1500, 390);
  TEST_ASSERT_FALSE(ack.accept(8, 1550));
  TEST_ASSERT_TRUE(ack.accept(16, 1889));
  ack.expect(24, 2000, 390);
  ack.clear();
  TEST_ASSERT_FALSE(ack.accept(24, 2300));
  ack.expect(32, 2500, 0);
  TEST_ASSERT_FALSE(ack.accept(32, 2500));
}

void test_ack_windows_handle_millis_and_sequence_wrap() {
  lora_schedule::AckWindow ack;
  const uint32_t start = UINT32_MAX - 100;
  ack.expect(65528, start, 390);
  TEST_ASSERT_TRUE(ack.accept(65528, start + 300));
  ack.expect(0, start + 4000, 390);
  TEST_ASSERT_FALSE(ack.accept(65528, start + 4200));
  TEST_ASSERT_TRUE(ack.accept(0, start + 4300));
  ack.expect(8, start + 8000, 390);
  TEST_ASSERT_FALSE(ack.accept(8, start + 8390));
}

void test_atpc_waits_until_expected_ack_is_received_or_window_expires() {
  lora_schedule::AckWindow ack;
  TEST_ASSERT_FALSE(ack.pending(1000));
  ack.expect(8, 1000, 390);
  TEST_ASSERT_TRUE(ack.pending(1165));  // DATA TX ended, ACK still on air
  TEST_ASSERT_TRUE(ack.pending(1389));
  TEST_ASSERT_FALSE(ack.pending(1390));
  ack.expect(16, 1500, 390);
  TEST_ASSERT_TRUE(ack.pending(1700));
  TEST_ASSERT_TRUE(ack.accept(16, 1810));
  TEST_ASSERT_FALSE(ack.pending(1811));
  const uint32_t start = UINT32_MAX - 100;
  ack.expect(24, start, 390);
  TEST_ASSERT_TRUE(ack.pending(start + 300));
  TEST_ASSERT_FALSE(ack.pending(start + 390));
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
  RUN_TEST(test_late_ack_cycle_is_skipped_before_transmit_can_collide);
  RUN_TEST(test_data_fit_accounts_for_ack_and_precise_remaining_guard);
  RUN_TEST(test_telemetry_respects_both_guards_and_exact_window_boundaries);
  RUN_TEST(test_telemetry_never_runs_on_ack_or_unfinished_data_cycle);
  RUN_TEST(test_telemetry_window_survives_wrap_and_late_data_start);
  RUN_TEST(test_ack_accepts_only_current_expected_seq_once);
  RUN_TEST(test_ack_expiry_replacement_and_clear);
  RUN_TEST(test_ack_windows_handle_millis_and_sequence_wrap);
  RUN_TEST(test_atpc_waits_until_expected_ack_is_received_or_window_expires);
  RUN_TEST(test_ring_retains_chronological_last_events_and_overwrite_count);
  RUN_TEST(test_ring_exports_in_insertion_order_across_millis_wrap);
  return UNITY_END();
}
