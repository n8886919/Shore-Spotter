#include <unity.h>
#include "packet_diagnostics.h"

using packet_diagnostics::Event;
using packet_diagnostics::Ring;
using packet_diagnostics::Selection;
using packet_diagnostics::selectWindow;

void setUp() {}
void tearDown() {}

template <size_t Capacity>
static void push(Ring<Capacity> &ring, unsigned count) {
  for (unsigned i = 0; i < count; ++i) ring.push(Event{});
}

static void assertPage(const Selection &page, size_t start, size_t count,
                       uint32_t nextId, bool more, bool dropped, bool reset) {
  TEST_ASSERT_EQUAL_UINT32(start, page.start);
  TEST_ASSERT_EQUAL_UINT32(count, page.count);
  TEST_ASSERT_EQUAL_UINT32(nextId, page.nextId);
  TEST_ASSERT_EQUAL(more, page.more);
  TEST_ASSERT_EQUAL(dropped, page.dropped);
  TEST_ASSERT_EQUAL(reset, page.reset);
}

void test_cold_empty_ring_returns_a_cursor_without_claiming_loss() {
  Ring<64> ring;
  assertPage(ring.select(false, false, 900), 0, 0, 0, false, false, true);
  assertPage(ring.select(true, true, 0), 0, 0, 0, false, false, false);
  // A future same-boot cursor is invalid even before the first event.
  assertPage(ring.select(true, true, 1), 0, 0, 0, false, true, true);
  push(ring, 1);
  assertPage(ring.select(true, true, 0), 0, 1, 1, false, false, false);
}

void test_cold_cursor_pages_from_oldest_and_never_skips_unsent_events() {
  Ring<64> ring;
  push(ring, 19);
  Selection page = ring.select(false, true, 0);
  assertPage(page, 0, 8, 8, true, false, true);
  TEST_ASSERT_EQUAL_UINT32(1, ring.at(page.start).id);
  TEST_ASSERT_EQUAL_UINT32(page.nextId, ring.at(page.start + page.count - 1).id);
  page = ring.select(true, true, page.nextId);
  assertPage(page, 8, 8, 16, true, false, false);
  page = ring.select(true, true, page.nextId);
  assertPage(page, 16, 3, 19, false, false, false);
  page = ring.select(true, true, page.nextId);
  assertPage(page, 19, 0, 19, false, false, false);
}

void test_arrivals_between_pages_are_returned_once_in_order() {
  Ring<16> ring;
  push(ring, 10);
  Selection page = ring.select(false, false, 0);
  assertPage(page, 0, 8, 8, true, false, true);
  push(ring, 2);
  page = ring.select(true, true, page.nextId, 3);
  assertPage(page, 8, 3, 11, true, false, false);
  TEST_ASSERT_EQUAL_UINT32(9, ring.at(page.start).id);
  TEST_ASSERT_EQUAL_UINT32(11, ring.at(page.start + 2).id);
  page = ring.select(true, true, page.nextId, 3);
  assertPage(page, 11, 1, 12, false, false, false);
  push(ring, 1);
  page = ring.select(true, true, page.nextId, 3);
  assertPage(page, 12, 1, 13, false, false, false);
}

void test_normal_ring_rotation_does_not_report_loss_of_already_read_events() {
  Ring<4> ring;
  push(ring, 4);
  Selection page = ring.select(false, false, 0, 3);
  assertPage(page, 0, 3, 3, true, false, true);
  push(ring, 2);  // retains IDs 3..6; consumed IDs 1..2 are overwritten
  page = ring.select(true, true, page.nextId, 3);
  assertPage(page, 1, 3, 6, false, false, false);
  TEST_ASSERT_EQUAL_UINT32(4, ring.at(page.start).id);
  TEST_ASSERT_EQUAL_UINT32(6, ring.at(page.start + page.count - 1).id);
}

void test_overrun_and_future_cursor_reset_to_oldest_with_loss() {
  Ring<4> ring;
  push(ring, 8);  // retained IDs 5..8
  Selection page = ring.select(true, true, 3, 2);
  assertPage(page, 0, 2, 6, true, true, true);
  TEST_ASSERT_EQUAL_UINT32(5, ring.at(page.start).id);
  page = ring.select(true, true, page.nextId, 2);
  assertPage(page, 2, 2, 8, false, false, false);
  assertPage(ring.select(true, true, 9, 2), 0, 2, 6, true, true, true);
  // The predecessor of the oldest retained ID is still a valid cursor.
  assertPage(ring.select(true, true, 4, 2), 0, 2, 6, true, false, false);
}

void test_boot_change_and_initial_history_do_not_claim_same_boot_loss() {
  Ring<4> ring;
  push(ring, 8);
  assertPage(ring.select(false, false, 0, 2), 0, 2, 6, true, false, true);
  assertPage(ring.select(true, false, UINT32_MAX, 2), 0, 2, 6, true, false, true);
  assertPage(ring.select(true, false, 8, 2), 0, 2, 6, true, false, true);
  Ring<4> empty;
  assertPage(empty.select(true, false, 800), 0, 0, 0, false, false, true);
}

void test_limit_is_bounded_and_small_pages_make_progress() {
  Ring<64> ring;
  push(ring, 16);
  assertPage(ring.select(false, false, 0, 1), 0, 1, 1, true, false, true);
  assertPage(ring.select(false, false, 0, 0), 0, 8, 8, true, false, true);
  assertPage(ring.select(false, false, 0, 1000), 0, 8, 8, true, false, true);
  assertPage(ring.select(true, true, 15, 8), 15, 1, 16, false, false, false);
}

void test_id_wrap_pages_through_zero_without_reset_or_replay() {
  // Retained IDs are UINT32_MAX-4 .. UINT32_MAX, 0, 1, 2.
  Selection page = selectWindow(2, 8, true, true, UINT32_MAX - 2, 3);
  assertPage(page, 3, 3, 0, true, false, false);
  page = selectWindow(2, 8, true, true, page.nextId, 3);
  assertPage(page, 6, 2, 2, false, false, false);
  page = selectWindow(2, 8, true, true, page.nextId, 3);
  assertPage(page, 8, 0, 2, false, false, false);
  assertPage(selectWindow(0, 8, true, true, 0), 8, 0, 0, false, false, false);
  assertPage(selectWindow(0, 8, true, true, UINT32_MAX), 7, 1, 0, false, false, false);
}

void test_id_wrap_handles_initial_history_overrun_and_future() {
  assertPage(selectWindow(2, 8, false, false, 0, 3),
             0, 3, UINT32_MAX - 2, true, false, true);
  // Exactly the oldest predecessor is valid; one ID earlier lost an event.
  assertPage(selectWindow(2, 8, true, true, UINT32_MAX - 5, 3),
             0, 3, UINT32_MAX - 2, true, false, false);
  assertPage(selectWindow(2, 8, true, true, UINT32_MAX - 6, 3),
             0, 3, UINT32_MAX - 2, true, true, true);
  assertPage(selectWindow(2, 8, true, true, 3, 3),
             0, 3, UINT32_MAX - 2, true, true, true);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_cold_empty_ring_returns_a_cursor_without_claiming_loss);
  RUN_TEST(test_cold_cursor_pages_from_oldest_and_never_skips_unsent_events);
  RUN_TEST(test_arrivals_between_pages_are_returned_once_in_order);
  RUN_TEST(test_normal_ring_rotation_does_not_report_loss_of_already_read_events);
  RUN_TEST(test_overrun_and_future_cursor_reset_to_oldest_with_loss);
  RUN_TEST(test_boot_change_and_initial_history_do_not_claim_same_boot_loss);
  RUN_TEST(test_limit_is_bounded_and_small_pages_make_progress);
  RUN_TEST(test_id_wrap_pages_through_zero_without_reset_or_replay);
  RUN_TEST(test_id_wrap_handles_initial_history_overrun_and_future);
  return UNITY_END();
}
