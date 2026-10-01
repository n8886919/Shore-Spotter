#include <unity.h>
#include "t096_control.h"
#include "t096_radio_config.h"

static PacketHeader commandHeader(uint16_t sequence) { return {0x20, sequence, MSG_CLIENT_CONTROL}; }
static client_control::Command command(uint32_t boot, client_control::Action action, uint16_t station) {
  return {boot, action, station};
}

void test_default_ready_period_and_storage_wrap_are_bounded() {
  t096_control::Controller control(77); control.setClientId(0x20);
  control.resetReadyTimer(0xFFFF0000U);
  TEST_ASSERT_FALSE(control.storageDue(0x0000FFFFU, false));
  TEST_ASSERT_TRUE(control.storageDue(0xFFFF0000U + client_control::kStorageAfterMs, false));
  TEST_ASSERT_FALSE(control.storageDue(0xFFFF0000U + client_control::kStorageAfterMs, true));
  TEST_ASSERT_TRUE(control.pollDue(client_control::kReadyPollMs));
  control.notedState(client_control::kReadyPollMs);
  TEST_ASSERT_FALSE(control.pollDue(client_control::kReadyPollMs + 1));
}

void test_charger_removal_restarts_ready_12h_budget() {
  t096_control::Controller control(77); control.setClientId(0x20);
  control.resetReadyTimer(100);
  TEST_ASSERT_TRUE(control.storageDue(100 + client_control::kStorageAfterMs, false));
  // The application calls this on VBUS falling; it must not carry over the
  // time that USB power inhibited SystemOFF.
  control.resetReadyTimer(500);
  TEST_ASSERT_FALSE(control.storageDue(500 + client_control::kStorageAfterMs - 1, false));
  TEST_ASSERT_TRUE(control.storageDue(500 + client_control::kStorageAfterMs, false));
}

void test_boot_station_and_old_command_are_rejected() {
  t096_control::Controller control(77); control.setClientId(0x20);
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Ignored,
                    control.apply(commandHeader(1), command(78, client_control::Action::Start, 10), 1));
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Ignored,
                    control.apply({0x21, 1, MSG_CLIENT_CONTROL}, command(77, client_control::Action::Start, 10), 1));
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Applied,
                    control.apply(commandHeader(10), command(77, client_control::Action::Start, 10), 2));
  TEST_ASSERT_EQUAL(client_control::State::Tracking, control.state());
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Ignored,
                    control.apply(commandHeader(9), command(77, client_control::Action::Stop, 10), 3));
  TEST_ASSERT_EQUAL(client_control::State::Tracking, control.state());
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Ignored,
                    control.apply(commandHeader(11), command(77, client_control::Action::Stop, 11), 4));
}

void test_duplicate_confirms_without_restarting_ready_timer() {
  t096_control::Controller control(77); control.setClientId(0x20);
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Applied,
                    control.apply(commandHeader(65535), command(77, client_control::Action::Stop, 10), 100));
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Duplicate,
                    control.apply(commandHeader(65535), command(77, client_control::Action::Stop, 10), 1000));
  TEST_ASSERT_FALSE(control.storageDue(100 + client_control::kStorageAfterMs - 1, false));
  TEST_ASSERT_TRUE(control.storageDue(100 + client_control::kStorageAfterMs, false));
  TEST_ASSERT_EQUAL(t096_control::CommandResult::Applied,
                    control.apply(commandHeader(0), command(77, client_control::Action::Test, 10), 200));
  TEST_ASSERT_TRUE(control.probeDue(700));
}

struct FakeSx1262 {
  float frequency = 0, bandwidth = 0, tcxo = 0;
  uint8_t sf = 0, cr = 0, sync = 0;
  int8_t drive = 0;
  uint16_t preamble = 0;
  int16_t begin(float f, float b, uint8_t newSf, uint8_t newCr, uint8_t newSync,
                int8_t newDrive, uint16_t newPreamble, float newTcxo) {
    frequency = f; bandwidth = b; sf = newSf; cr = newCr; sync = newSync;
    drive = newDrive; preamble = newPreamble; tcxo = newTcxo;
    return 0;
  }
};

void test_sx1262_begin_keeps_preamble_and_tcxo_in_their_real_api_slots() {
  FakeSx1262 radio;
  TEST_ASSERT_EQUAL_INT16(0, t096_radio::begin(radio, 923.8f, 125.0f, 10, 5, 0x12, 0));
  TEST_ASSERT_EQUAL_UINT16(8, radio.preamble);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.8f, radio.tcxo);
  TEST_ASSERT_EQUAL_INT8(0, radio.drive);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_ready_period_and_storage_wrap_are_bounded);
  RUN_TEST(test_charger_removal_restarts_ready_12h_budget);
  RUN_TEST(test_boot_station_and_old_command_are_rejected);
  RUN_TEST(test_duplicate_confirms_without_restarting_ready_timer);
  RUN_TEST(test_sx1262_begin_keeps_preamble_and_tcxo_in_their_real_api_slots);
  return UNITY_END();
}
