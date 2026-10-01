#include <unity.h>
#include <cmath>
#include <limits>
#include "phone_position.h"
#include "client_control.h"
#include "station_client_link.h"

static PacketHeader header(uint16_t id, uint16_t seq, uint8_t type) { return {id, seq, type}; }

void test_phone_rejects_nan_and_older_snapshot() {
  phone_position::Fix fix;
  TEST_ASSERT_TRUE(fix.set(24.1, 121.5, 8.0, 2000000000000.0, 100));
  TEST_ASSERT_FALSE(fix.set(24.2, 121.6, 8.0, 1999999999999.0, 101));
  TEST_ASSERT_FALSE(fix.set(std::numeric_limits<double>::quiet_NaN(), 121.6, 8.0, 2000000000001.0, 102));
  TEST_ASSERT_FALSE(fix.set(24.2, 121.6, -1.0, 2000000000001.0, 102));
  TEST_ASSERT_EQUAL_UINT32(9, fix.age(109));
}

void test_client_control_codecs_require_exact_lengths_and_reserved_values() {
  uint8_t buffer[32]{}; PacketHeader parsed{};
  client_control::Command command{7, client_control::Action::Start, 22}, decodedCommand{};
  TEST_ASSERT_EQUAL_UINT(CLIENT_CONTROL_PACKET_LEN, client_control::encodeCommand(buffer, sizeof(buffer), header(5, 1, MSG_CLIENT_CONTROL), command));
  TEST_ASSERT_TRUE(client_control::decodeCommand(buffer, CLIENT_CONTROL_PACKET_LEN, parsed, decodedCommand));
  TEST_ASSERT_EQUAL_UINT32(7, decodedCommand.boot); TEST_ASSERT_EQUAL_UINT8(uint8_t(client_control::Action::Start), uint8_t(decodedCommand.action));
  TEST_ASSERT_FALSE(client_control::decodeCommand(buffer, CLIENT_CONTROL_PACKET_LEN - 1, parsed, decodedCommand));

  client_control::Status status{8, client_control::State::Ready, 22, 9, 10, 3900, 11, 1, true}, decodedStatus{};
  TEST_ASSERT_EQUAL_UINT(CLIENT_STATE_PACKET_LEN, client_control::encodeStatus(buffer, sizeof(buffer), header(5, 2, MSG_CLIENT_STATE), status));
  TEST_ASSERT_TRUE(client_control::decodeStatus(buffer, CLIENT_STATE_PACKET_LEN, parsed, decodedStatus));
  TEST_ASSERT_FALSE(client_control::decodeStatus(buffer, CLIENT_STATE_PACKET_LEN + 1, parsed, decodedStatus));
  buffer[27] = 2; TEST_ASSERT_FALSE(client_control::decodeStatus(buffer, CLIENT_STATE_PACKET_LEN, parsed, decodedStatus));

  client_control::Probe probe{9, 10, 11}, decodedProbe{};
  TEST_ASSERT_EQUAL_UINT(LINK_TEST_PACKET_LEN, client_control::encodeProbe(buffer, sizeof(buffer), header(5, 3, MSG_LINK_TEST), probe));
  TEST_ASSERT_TRUE(client_control::decodeProbe(buffer, LINK_TEST_PACKET_LEN, parsed, decodedProbe));
  TEST_ASSERT_FALSE(client_control::decodeProbe(buffer, LINK_TEST_PACKET_LEN - 1, parsed, decodedProbe));
}

static client_control::Status state(uint32_t boot, client_control::State value, uint16_t station, uint16_t command) {
  return {boot, value, station, command, 0, 3900, 0, 0, false};
}

void test_link_confirms_only_matching_boot_station_and_command() {
  station_client_link::Link link;
  link.observe(5, state(100, client_control::State::Ready, 42, 7), 1, 42);
  TEST_ASSERT_TRUE(link.request(client_control::Action::Start, 2, 42));
  TEST_ASSERT_EQUAL_STRING("pending", link.command); TEST_ASSERT_EQUAL_UINT16(8, link.commandId);
  link.observe(5, state(101, client_control::State::Tracking, 42, 8), 3, 42);
  TEST_ASSERT_EQUAL_STRING("error", link.command);  // reboot invalidates the in-flight command

  link = station_client_link::Link{};
  link.observe(5, state(100, client_control::State::Ready, 42, 7), 1, 42);
  TEST_ASSERT_TRUE(link.request(client_control::Action::Start, 2, 42));
  link.observe(5, state(100, client_control::State::Tracking, 41, link.commandId), 3, 42);
  TEST_ASSERT_EQUAL_STRING("pending", link.command);  // foreign station cannot confirm it
  link.tick(2 + client_control::kCommandTimeoutMs);
  TEST_ASSERT_EQUAL_STRING("timeout", link.command);

  link = station_client_link::Link{};
  link.observe(5, state(100, client_control::State::Ready, 42, 7), 1, 42);
  TEST_ASSERT_TRUE(link.request(client_control::Action::Start, 2, 42));
  link.observe(5, state(100, client_control::State::Tracking, 42, link.commandId), 3, 42);
  TEST_ASSERT_EQUAL_STRING("confirmed", link.command);
  TEST_ASSERT_FALSE(link.request(client_control::Action::Stop, 4, 99));  // paired elsewhere
}

void test_probe_rejects_duplicates_and_accepts_counter_wrap() {
  station_client_link::Link link;
  TEST_ASSERT_TRUE(link.probe({7, 0xFFFFFFFEu, 1}, 10, -70, 5));
  TEST_ASSERT_TRUE(link.probe({7, 0xFFFFFFFFu, 2}, 20, -71, 6));
  TEST_ASSERT_TRUE(link.probe({7, 0u, 3}, 30, -72, 7));
  TEST_ASSERT_EQUAL_UINT32(3, link.probes); TEST_ASSERT_EQUAL_UINT32(0, link.missing);
  TEST_ASSERT_FALSE(link.probe({7, 0u, 4}, 40, -73, 8));
  TEST_ASSERT_FALSE(link.probe({7, 0xFFFFFFFFu, 5}, 50, -74, 9));
  TEST_ASSERT_EQUAL_UINT32(3, link.probes);
}

void setUp() {}
void tearDown() {}
int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_phone_rejects_nan_and_older_snapshot);
  RUN_TEST(test_client_control_codecs_require_exact_lengths_and_reserved_values);
  RUN_TEST(test_link_confirms_only_matching_boot_station_and_command);
  RUN_TEST(test_probe_rejects_duplicates_and_accepts_counter_wrap);
  return UNITY_END();
}
