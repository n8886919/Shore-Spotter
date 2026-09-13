#include <unity.h>
#include "async_lora_ack.h"
#include "client_binding.h"
#include "magnetic_declination.h"

void setUp() {}
void tearDown() {}

struct FakeRadio {
  volatile bool irq = false;
  uint32_t flags = 0;
  int starts = 0, finishes = 0, receives = 0, reads = 0;
  int16_t startStatus = 0, finishStatus = 0, receiveStatus = 0;
  bool irqDuringReceive = false;
  int16_t startTransmit(const uint8_t *, size_t) { ++starts; return startStatus; }
  uint32_t getIrqFlags() { ++reads; return flags; }
  int16_t finishTransmit() { ++finishes; return finishStatus; }
  int16_t startReceive() { ++receives; irq = irqDuringReceive; return receiveStatus; }
};
using Ack = async_lora_ack::Transmitter<FakeRadio>;
using Event = async_lora_ack::Event;
const uint8_t packet[] = {1, 2, 3};

void test_ack_returns_to_caller_until_tx_done() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  radio.irq = true;  // old RxDone must not finish the new TX
  TEST_ASSERT_TRUE(ack.start(packet, sizeof(packet), 100, 500).event == Event::Started);
  TEST_ASSERT_FALSE(radio.irq);
  for (uint32_t now = 101; now < 500; ++now)
    TEST_ASSERT_TRUE(ack.service(now).event == Event::None);
  TEST_ASSERT_TRUE(ack.active());
  TEST_ASSERT_EQUAL(0, radio.reads);
  TEST_ASSERT_EQUAL(0, radio.receives);
  TEST_ASSERT_TRUE(ack.start(packet, sizeof(packet), 500, 500).event == Event::None);
  TEST_ASSERT_EQUAL(1, radio.starts);
  radio.flags = 1;
  radio.irq = true;
  TEST_ASSERT_TRUE(ack.service(501).event == Event::Sent);
  TEST_ASSERT_FALSE(ack.active());
  TEST_ASSERT_EQUAL(1, radio.finishes);
  TEST_ASSERT_EQUAL(1, radio.receives);
  TEST_ASSERT_FALSE(radio.irq);
  TEST_ASSERT_TRUE(ack.service(502).event == Event::None);
  TEST_ASSERT_EQUAL(1, radio.receives);
}

void test_ack_preserves_fresh_rx_irq_and_ignores_unrelated_irq() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  ack.start(packet, sizeof(packet), 0, 500);
  radio.flags = 4;
  radio.irq = true;
  TEST_ASSERT_TRUE(ack.service(50).event == Event::None);
  TEST_ASSERT_TRUE(ack.active());
  radio.flags = 1;
  radio.irq = true;
  radio.irqDuringReceive = true;
  TEST_ASSERT_TRUE(ack.service(100).event == Event::Sent);
  TEST_ASSERT_TRUE(radio.irq);  // new RxDone belongs to main RX handler
}

void test_ack_timeouts_and_rollover() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  const uint32_t start = UINT32_MAX - 100;
  ack.start(packet, sizeof(packet), start, 500);
  TEST_ASSERT_TRUE(ack.service(start + 499).event == Event::None);
  auto result = ack.service(start + 500);
  TEST_ASSERT_TRUE(result.event == Event::Timeout);
  TEST_ASSERT_EQUAL(-5, result.txStatus);
  TEST_ASSERT_EQUAL(1, radio.receives);
  ack.start(packet, sizeof(packet), 500, 500);
  radio.flags = 2;
  radio.irq = true;
  TEST_ASSERT_TRUE(ack.service(501).event == Event::Timeout);
  TEST_ASSERT_FALSE(ack.active());
}

void test_ack_late_service_prefers_tx_done() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  ack.start(packet, sizeof(packet), 0, 100);
  radio.flags = 1;
  // The hardware completed TX even if ISR notification was missed.
  TEST_ASSERT_TRUE(ack.service(150).event == Event::Sent);
}

void test_ack_failures_restore_rx_and_report_errors() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  radio.startStatus = -7;
  radio.receiveStatus = -8;
  auto result = ack.start(packet, sizeof(packet), 0, 100);
  TEST_ASSERT_TRUE(result.event == Event::Failed);
  TEST_ASSERT_EQUAL(-7, result.txStatus);
  TEST_ASSERT_EQUAL(-8, result.rxStatus);
  TEST_ASSERT_FALSE(ack.active());
  TEST_ASSERT_EQUAL(1, radio.finishes);
  TEST_ASSERT_EQUAL(1, radio.receives);
  radio.startStatus = radio.receiveStatus = 0;
  radio.finishStatus = -9;
  ack.start(packet, sizeof(packet), 100, 100);
  radio.flags = 1;
  radio.irq = true;
  result = ack.service(101);
  TEST_ASSERT_TRUE(result.event == Event::Failed);
  TEST_ASSERT_EQUAL(-9, result.txStatus);
  TEST_ASSERT_EQUAL(0, result.rxStatus);
}

void test_ack_cancel_once_for_client_replacement() {
  FakeRadio radio;
  Ack ack(radio, radio.irq, 1, 2, -5);
  ack.start(packet, sizeof(packet), 0, 500);
  TEST_ASSERT_TRUE(ack.cancel().event == Event::Cancelled);
  TEST_ASSERT_TRUE(ack.cancel().event == Event::None);
  TEST_ASSERT_FALSE(ack.active());
  TEST_ASSERT_EQUAL(1, radio.receives);
}

void test_client_binding_strict_id_and_reserved_values() {
  uint16_t id = 0;
  TEST_ASSERT_TRUE(client_binding::parseId("aB12", 4, id));
  TEST_ASSERT_EQUAL_HEX16(0xAB12, id);
  TEST_ASSERT_FALSE(client_binding::parseId("", 0, id));
  TEST_ASSERT_FALSE(client_binding::parseId("12345", 5, id));
  TEST_ASSERT_FALSE(client_binding::parseId("1x", 2, id));
  TEST_ASSERT_FALSE(client_binding::parseId(" 123", 4, id));
  TEST_ASSERT_FALSE(client_binding::parseId("-1", 2, id));
  TEST_ASSERT_FALSE(client_binding::validId(0));
  TEST_ASSERT_FALSE(client_binding::validId(SERVER_ID));
  TEST_ASSERT_FALSE(client_binding::validId(ID_BROADCAST));
}

void test_client_binding_migrates_first_only_and_preserves_empty() {
  TEST_ASSERT_EQUAL_HEX16(0xAB12, client_binding::fromLegacyList("AB12,CD34", 9));
  TEST_ASSERT_EQUAL_HEX16(0xAB12, client_binding::fromLegacyList("0000,AB12", 9));
  TEST_ASSERT_EQUAL_HEX16(0, client_binding::fromLegacyList("", 0));
  TEST_ASSERT_EQUAL_HEX16(0, client_binding::fromLegacyList("bad-id,0000", 11));
}

void test_declination_date_and_model_lifespan() {
  float year = 0;
  TEST_ASSERT_TRUE(magnetic_declination::decimalYear(2025, 1, 1, year));
  TEST_ASSERT_EQUAL_FLOAT(2025, year);
  TEST_ASSERT_TRUE(magnetic_declination::decimalYear(2028, 2, 29, year));
  TEST_ASSERT_FLOAT_WITHIN(0.0002f, 2028 + 59.0f / 366, year);
  TEST_ASSERT_TRUE(magnetic_declination::decimalYear(2028, 3, 1, year));
  TEST_ASSERT_FLOAT_WITHIN(0.0002f, 2028 + 60.0f / 366, year);
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2026, 2, 29, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2026, 4, 31, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2026, 0, 1, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2026, 13, 1, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2026, 1, 0, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2024, 12, 31, year));
  TEST_ASSERT_FALSE(magnetic_declination::decimalYear(2030, 1, 1, year));
}

void test_declination_taiwan_grid_bounds_and_reference() {
  float d = 0;
  // Independent full WMM2025 calculation, Taipei vicinity, sea level.
  TEST_ASSERT_TRUE(magnetic_declination::taiwanDegrees(25, 121.5, 2026.687f, d));
  TEST_ASSERT_FLOAT_WITHIN(0.004f, -5.042737f, d);
  TEST_ASSERT_TRUE(magnetic_declination::taiwanDegrees(18, 116, 2025, d));
  TEST_ASSERT_TRUE(magnetic_declination::taiwanDegrees(28, 124, 2029.999f, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(17.9, 121, 2026, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(23, 124.1, 2026, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(NAN, 121, 2026, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(23, INFINITY, 2026, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(23, 121, NAN, d));
  TEST_ASSERT_FALSE(magnetic_declination::taiwanDegrees(23, 121, 2030, d));
}

void test_magnetic_to_true_bearing_sign_and_wrap() {
  TEST_ASSERT_EQUAL_FLOAT(355, magnetic_declination::trueBearing(0, -5));
  TEST_ASSERT_EQUAL_FLOAT(85, magnetic_declination::trueBearing(90, -5));
  TEST_ASSERT_EQUAL_FLOAT(2, magnetic_declination::trueBearing(359, 3));
  // CCW-positive servo at 90 deg, camera compass 0 deg, true target 355 deg:
  // applying declination must preserve the calibrated servo angle.
  float target = magnetic_declination::trueBearing(0 + 90, -5) - 355;
  if (target < 0) target += 360;
  TEST_ASSERT_EQUAL_FLOAT(90, target);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ack_returns_to_caller_until_tx_done);
  RUN_TEST(test_ack_preserves_fresh_rx_irq_and_ignores_unrelated_irq);
  RUN_TEST(test_ack_timeouts_and_rollover);
  RUN_TEST(test_ack_late_service_prefers_tx_done);
  RUN_TEST(test_ack_failures_restore_rx_and_report_errors);
  RUN_TEST(test_ack_cancel_once_for_client_replacement);
  RUN_TEST(test_client_binding_strict_id_and_reserved_values);
  RUN_TEST(test_client_binding_migrates_first_only_and_preserves_empty);
  RUN_TEST(test_declination_date_and_model_lifespan);
  RUN_TEST(test_declination_taiwan_grid_bounds_and_reference);
  RUN_TEST(test_magnetic_to_true_bearing_sign_and_wrap);
  return UNITY_END();
}
