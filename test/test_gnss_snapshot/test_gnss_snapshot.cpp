#include <unity.h>
#include <string>
#include <stdio.h>
#include "gnss_snapshot.h"

using gnss_snapshot::Collector;
using gnss_snapshot::Snapshot;
void setUp() {}
void tearDown() {}

static std::string sentence(const std::string &body) {
  unsigned checksum = 0;
  for (char c : body) checksum ^= static_cast<unsigned char>(c);
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  return "$" + body + suffix;
}
static std::string rmc(const char *utc = "120000.000", const char *status = "A",
                       const char *speed = "10.00", const char *course = "90.00") {
  return std::string("GNRMC,") + utc + "," + status +
      ",2400.00000,N,12100.00000,E," + speed + "," + course + ",130926,,,A";
}
static std::string gga(const char *utc = "120000.000", const char *quality = "1",
                       const char *sats = "08", const char *hdop = "0.9") {
  return std::string("GNGGA,") + utc + ",2400.00000,N,12100.00000,E," +
      quality + "," + sats + "," + hdop + ",10.0,M,0.0,M,,";
}
static void feed(Collector &c, const std::string &body, uint32_t now) {
  for (char ch : sentence(body)) c.feed(ch, now);
}
static Snapshot sample(Collector &c, uint32_t now) {
  Snapshot s;
  TEST_ASSERT_TRUE(c.sample(now, s));
  return s;
}

void test_matching_epoch_joins_rmc_and_gga_in_both_orders() {
  for (unsigned reverse = 0; reverse < 2; ++reverse) {
    Collector c;
    feed(c, reverse ? gga() : rmc(), 1000);
    feed(c, reverse ? rmc() : gga(), 1100);
    const Snapshot s = sample(c, 1200);
    TEST_ASSERT_TRUE(s.haveEpoch && s.haveRmc && s.haveGga);
    TEST_ASSERT_TRUE(s.fix && s.velocityValid);
    TEST_ASSERT_EQUAL_UINT32(43200000, s.epochMsOfDay);
    TEST_ASSERT_EQUAL_UINT8(8, s.satellites);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 24.0, s.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 121.0, s.lon);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1852.0 / 360.0, s.speedMps);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 90.0, s.courseDeg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.9, s.hdop);
    TEST_ASSERT_TRUE(s.sourceAgeMs >= 200);
    TEST_ASSERT_EQUAL_UINT32(200, s.ageUncertaintyMs);
    TEST_ASSERT_EQUAL_UINT32(0, c.counters().duplicateEpochs);
  }
}

void test_rmc_only_and_gga_only_do_not_invent_missing_fields() {
  Collector c;
  feed(c, rmc(), 1000);
  Snapshot s = sample(c, 1000);
  TEST_ASSERT_TRUE(s.fix && s.velocityValid);
  TEST_ASSERT_EQUAL_UINT8(255, s.satellites);
  TEST_ASSERT_TRUE(isnan(s.hdop));
  feed(c, gga("120001.000"), 2000);
  s = sample(c, 2000);
  TEST_ASSERT_TRUE(s.fix);
  TEST_ASSERT_FALSE(s.velocityValid || s.haveRmc);
  TEST_ASSERT_EQUAL_UINT8(8, s.satellites);
}

void test_epoch_mismatch_never_reuses_old_velocity_or_quality() {
  Collector c;
  feed(c, rmc(), 1000);
  feed(c, gga(), 1100);
  feed(c, gga("120001.000"), 2000);
  Snapshot s = sample(c, 2000);
  TEST_ASSERT_FALSE(s.velocityValid);
  feed(c, rmc("120002.000"), 3000);
  s = sample(c, 3000);
  TEST_ASSERT_FALSE(s.velocityValid);
  TEST_ASSERT_EQUAL_UINT32(43201000, s.epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT8(8, s.satellites);
  feed(c, rmc("120001.000"), 3100);  // interleaved older RMC
  s = sample(c, 3100);
  TEST_ASSERT_EQUAL_UINT32(43201000, s.epochMsOfDay);
  feed(c, rmc("120003.000"), 4000);  // GGA still absent, no timeless cached quality
  s = sample(c, 4000);
  TEST_ASSERT_EQUAL_UINT32(43203000, s.epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT8(255, s.satellites);
}

void test_rmc_gga_gap_keeps_aged_coherent_quality_and_invalidity_wins() {
  Collector c;
  feed(c, rmc(), 1000);
  feed(c, gga(), 1100);
  const uint32_t initialAge = sample(c, 1100).sourceAgeMs;
  feed(c, rmc("120001.000"), 2000);
  Snapshot s = sample(c, 2000);  // 2 Hz TX happens during the RMC/GGA gap
  TEST_ASSERT_TRUE(s.fix && s.velocityValid && s.haveRmc && s.haveGga);
  TEST_ASSERT_EQUAL_UINT32(43200000, s.epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT8(8, s.satellites);
  TEST_ASSERT_EQUAL_UINT32(initialAge + 900, s.sourceAgeMs);
  feed(c, gga("120001.000"), 2100);
  TEST_ASSERT_EQUAL_UINT32(43201000, sample(c, 2100).epochMsOfDay);
  feed(c, rmc("120002.000", "V"), 3000);
  s = sample(c, 3000);
  TEST_ASSERT_EQUAL_UINT32(43202000, s.epochMsOfDay);
  TEST_ASSERT_FALSE(s.fix);
  feed(c, gga("120003.000", "0"), 4000);
  TEST_ASSERT_FALSE(sample(c, 4000).fix);
}

void test_repeated_epoch_does_not_refresh_age() {
  Collector c;
  feed(c, rmc(), 1000);
  const uint32_t initial = sample(c, 1000).sourceAgeMs;
  feed(c, rmc(), 1600);
  TEST_ASSERT_EQUAL_UINT32(initial + 600, sample(c, 1600).sourceAgeMs);
  feed(c, gga(), 1800);
  TEST_ASSERT_EQUAL_UINT32(initial + 800, sample(c, 1800).sourceAgeMs);
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().duplicateEpochs);
}

void test_source_age_includes_serialization_but_not_uncertainty() {
  Collector c;
  const std::string body = rmc();
  feed(c, body, 1000);
  const uint32_t serialized = (sentence(body).size() * 10000 + 9599) / 9600;
  Snapshot s = sample(c, 1000);
  TEST_ASSERT_EQUAL_UINT32(serialized, s.sourceAgeMs);
  TEST_ASSERT_EQUAL_UINT32(200, s.ageUncertaintyMs);
  TEST_ASSERT_TRUE(s.sourceAgeMs < 200);
}

void test_utc_anchor_keeps_late_delivery_old() {
  Collector c;
  feed(c, rmc(), 1000);
  const uint32_t initial = sample(c, 1000).sourceAgeMs;
  feed(c, rmc("120001.000"), 2600);  // local delivery delayed 600 ms
  TEST_ASSERT_EQUAL_UINT32(initial + 600, sample(c, 2600).sourceAgeMs);
  TEST_ASSERT_EQUAL_UINT32(1000, c.counters().lastEpochIntervalMs);
}

void test_invalid_same_epoch_sentence_vetoes_fix_in_both_orders() {
  for (unsigned reverse = 0; reverse < 2; ++reverse) {
    Collector c;
    feed(c, reverse ? gga() : rmc("120000.000", "V"), 1000);
    feed(c, reverse ? rmc("120000.000", "V") : gga(), 1100);
    TEST_ASSERT_FALSE(sample(c, 1100).fix);
    TEST_ASSERT_FALSE(sample(c, 1100).velocityValid);
    feed(c, rmc(), 1200);  // duplicate good data cannot erase invalid epoch veto
    TEST_ASSERT_FALSE(sample(c, 1200).fix);
  }
  Collector c;
  feed(c, rmc(), 1000);
  feed(c, gga("120000.000", "0"), 1100);
  TEST_ASSERT_FALSE(sample(c, 1100).fix);
  feed(c, gga("120001.000", "6"), 2000);  // estimated/dead reckoning
  TEST_ASSERT_FALSE(sample(c, 2000).fix);
}

void test_missing_or_invalid_velocity_does_not_destroy_position() {
  const char *courses[] = {"", "360", "409.5", "nan", "-1"};
  for (const char *course : courses) {
    Collector c;
    feed(c, rmc("120000.000", "A", "10", course), 1000);
    Snapshot s = sample(c, 1000);
    TEST_ASSERT_TRUE(s.fix);
    TEST_ASSERT_FALSE(s.velocityValid);
  }
  Collector c;
  feed(c, rmc("120000.000", "A", "", "90"), 1000);
  TEST_ASSERT_FALSE(sample(c, 1000).velocityValid);
}

void test_missing_quality_does_not_reuse_previous_epoch_quality() {
  Collector c;
  feed(c, gga(), 1000);
  feed(c, gga("120001.000", "1", "", ""), 2000);
  const Snapshot s = sample(c, 2000);
  TEST_ASSERT_TRUE(s.fix);
  TEST_ASSERT_EQUAL_UINT8(255, s.satellites);
  TEST_ASSERT_TRUE(isnan(s.hdop));
}

void test_checksum_overflow_and_junk_do_not_create_snapshot() {
  Collector c;
  Snapshot s;
  std::string broken = sentence(rmc());
  broken[broken.size() - 4] = broken[broken.size() - 4] == '0' ? '1' : '0';
  for (char ch : broken) c.feed(ch, 1000);
  TEST_ASSERT_FALSE(c.sample(1000, s));
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().checksumErrors);
  const std::string tooLong = "$GNRMC," + std::string(200, '1') + "*00\r\n";
  for (char ch : tooLong) c.feed(ch, 1100);
  TEST_ASSERT_FALSE(c.sample(1100, s));
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().overflows);
  for (char ch : std::string("GNRMC,garbage\r\n")) c.feed(ch, 1200);
  TEST_ASSERT_FALSE(c.sample(1200, s));
  feed(c, rmc(), 1300);
  TEST_ASSERT_TRUE(c.sample(1300, s));
}

void test_invalidate_discards_partial_and_prevents_duplicate_resurrection() {
  Collector c;
  feed(c, rmc(), 1000);
  const std::string next = sentence(gga("120001.000"));
  for (size_t i = 0; i < next.size() / 2; ++i) c.feed(next[i], 1100);
  c.invalidate(1400);
  for (size_t i = next.size() / 2; i < next.size(); ++i) c.feed(next[i], 1400);
  Snapshot s;
  TEST_ASSERT_FALSE(c.sample(1400, s));
  feed(c, rmc(), 1500);
  TEST_ASSERT_FALSE(c.sample(1500, s));
  feed(c, gga("120001.000"), 2000);
  TEST_ASSERT_TRUE(c.sample(2000, s));
  TEST_ASSERT_TRUE(s.fix);
  TEST_ASSERT_FALSE(s.velocityValid);
}

void test_backwards_utc_requires_explicit_reset() {
  Collector c;
  feed(c, rmc("120005.000"), 1000);
  feed(c, rmc("120004.000"), 1500);
  TEST_ASSERT_EQUAL_UINT32(43205000, sample(c, 1500).epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().backwardEpochs);
  c.reset(1600);
  feed(c, rmc("120004.000"), 1700);
  TEST_ASSERT_EQUAL_UINT32(43204000, sample(c, 1700).epochMsOfDay);
}

void test_midnight_and_local_millis_wrap() {
  Collector c;
  const uint32_t start = UINT32_MAX - 300;
  feed(c, rmc("235959.500"), start);
  const uint32_t age = sample(c, start).sourceAgeMs;
  feed(c, rmc("000000.000"), start + 500);
  const Snapshot s = sample(c, start + 600);
  TEST_ASSERT_EQUAL_UINT32(0, s.epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT32(age + 100, s.sourceAgeMs);
  TEST_ASSERT_EQUAL_UINT32(500, c.counters().lastEpochIntervalMs);
}

void test_gp_talker_and_coordinate_time_range_validation() {
  Collector c;
  std::string body = rmc();
  body[1] = 'P';
  feed(c, body, 1000);
  TEST_ASSERT_TRUE(sample(c, 1000).fix);
  const char *badTimes[] = {"240000.000", "120060.000", "120000.0001", "1200", "120000."};
  for (const char *utc : badTimes) feed(c, rmc(utc), 1100);
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().acceptedSentences);
  body = "GPRMC,120001.000,A,2460.00000,N,12100.00000,E,10,90,130926,,,A";
  feed(c, body, 2000);
  TEST_ASSERT_EQUAL_UINT32(43200000, sample(c, 2000).epochMsOfDay);
  TEST_ASSERT_EQUAL_UINT32(1, c.counters().acceptedSentences);
}

void test_other_nmea_types_are_not_reported_as_decode_errors() {
  Collector c;
  feed(c, "GPGSV,3,1,11,01,40,083,41,02,17,308,42,03,07,140,39,04,10,220,38", 1000);
  feed(c, "GNVTG,90.0,T,,M,10.0,N,18.52,K,A", 1000);
  feed(c, "GPTXT,01,01,02,ANTENNA OK", 1000);
  TEST_ASSERT_EQUAL_UINT32(3, c.counters().ignoredSentences);
  TEST_ASSERT_EQUAL_UINT32(0, c.counters().rejectedSentences);
  TEST_ASSERT_EQUAL_UINT32(0, c.counters().acceptedSentences);
  Snapshot s;
  TEST_ASSERT_FALSE(c.sample(1000, s));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_matching_epoch_joins_rmc_and_gga_in_both_orders);
  RUN_TEST(test_rmc_only_and_gga_only_do_not_invent_missing_fields);
  RUN_TEST(test_epoch_mismatch_never_reuses_old_velocity_or_quality);
  RUN_TEST(test_rmc_gga_gap_keeps_aged_coherent_quality_and_invalidity_wins);
  RUN_TEST(test_repeated_epoch_does_not_refresh_age);
  RUN_TEST(test_source_age_includes_serialization_but_not_uncertainty);
  RUN_TEST(test_utc_anchor_keeps_late_delivery_old);
  RUN_TEST(test_invalid_same_epoch_sentence_vetoes_fix_in_both_orders);
  RUN_TEST(test_missing_or_invalid_velocity_does_not_destroy_position);
  RUN_TEST(test_missing_quality_does_not_reuse_previous_epoch_quality);
  RUN_TEST(test_checksum_overflow_and_junk_do_not_create_snapshot);
  RUN_TEST(test_invalidate_discards_partial_and_prevents_duplicate_resurrection);
  RUN_TEST(test_backwards_utc_requires_explicit_reset);
  RUN_TEST(test_midnight_and_local_millis_wrap);
  RUN_TEST(test_gp_talker_and_coordinate_time_range_validation);
  RUN_TEST(test_other_nmea_types_are_not_reported_as_decode_errors);
  return UNITY_END();
}
