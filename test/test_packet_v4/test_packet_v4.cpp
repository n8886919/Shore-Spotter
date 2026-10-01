#include <unity.h>
#include <limits.h>
#include <initializer_list>
#include <string.h>
#include "protocol.h"
#include "command_freshness.h"

void setUp() {}
void tearDown() {}

static PacketHeader dataHeader(uint16_t seq = 0xFFFE) {
  return {0xAB12, seq, MSG_DATA};
}
static PositionPayload position() {
  return {241234560, 1203456790, 12, 1234, 3, true, true, 15};
}
static const uint8_t goldenData[] = {0x53,0x51,0x12,0xab,0xfe,0xff,0x80,0xf2,0x60,0xe,0x16,0x4b,0xbb,0x47,0xc,0xd2,0xf4,0xf};

void test_data_golden_bytes_define_wire_independent_of_struct_layout() {
  uint8_t buf[DATA_PACKET_LEN + 1]; memset(buf, 0xA5, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(18, protocol::encodeData(buf, DATA_PACKET_LEN, dataHeader(), position()));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(goldenData, buf, sizeof(goldenData));
  TEST_ASSERT_EQUAL_HEX8(0xA5, buf[DATA_PACKET_LEN]);
  PacketHeader h{}; PositionPayload p{};
  TEST_ASSERT_TRUE(protocol::decodeData(goldenData, sizeof(goldenData), h, p));
  TEST_ASSERT_EQUAL_HEX16(0xAB12, h.clientId);
  TEST_ASSERT_EQUAL_HEX16(0xFFFE, h.seq);
  TEST_ASSERT_EQUAL(MSG_DATA, h.msgType);
  TEST_ASSERT_EQUAL_INT32(241234560, p.latE7);
  TEST_ASSERT_EQUAL_INT32(1203456790, p.lonE7);
  TEST_ASSERT_EQUAL(12, p.speedDmS);
  TEST_ASSERT_EQUAL(1234, p.courseDeg10);
  TEST_ASSERT_EQUAL(3, p.satelliteClass);
  TEST_ASSERT_TRUE(p.fix); TEST_ASSERT_TRUE(p.velocityValid);
  TEST_ASSERT_EQUAL(15, p.hdop10);
}

void test_signed32_world_endpoints_round_trip() {
  uint8_t buf[DATA_PACKET_LEN];
  for (int32_t lat : {-900000000,-899999999,-1,0,1,899999999,900000000})
    for (int32_t lon : {-1800000000,-1799999999,-1,0,1,1799999999,1800000000}) {
      auto p=position();p.latE7=lat;p.lonE7=lon;
      TEST_ASSERT_EQUAL_UINT32(DATA_PACKET_LEN,protocol::encodeData(buf,sizeof(buf),dataHeader(),p));
      PacketHeader h{};PositionPayload d{};TEST_ASSERT_TRUE(protocol::decodeData(buf,sizeof(buf),h,d));
      TEST_ASSERT_EQUAL_INT32(lat,d.latE7);TEST_ASSERT_EQUAL_INT32(lon,d.lonE7);
    }
}

void test_coordinate_overflow_rejects_valid_fix_without_writing() {
  const int32_t badLat[] = {-900000001, 900000001, INT32_MIN, INT32_MAX};
  uint8_t buf[DATA_PACKET_LEN];
  for (int32_t lat : badLat) {
    memset(buf, 0xA5, sizeof(buf)); auto p = position(); p.latE7 = lat;
    TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
    for (uint8_t b : buf) TEST_ASSERT_EQUAL_HEX8(0xA5, b);
  }
  for (int32_t lon : {-1800000001, 1800000001, INT32_MIN, INT32_MAX}) {
    auto p = position(); p.lonE7 = lon;
    TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
  }
}

void test_no_fix_invalid_coordinates_are_canonical_not_wrapped() {
  auto p = position(); p.fix = false; p.velocityValid = false;
  p.latE7 = INT32_MAX; p.lonE7 = INT32_MIN;
  p.speedDmS = 255; p.courseDeg10 = 4095; p.satelliteClass = 0; p.hdop10 = 255;
  uint8_t buf[DATA_PACKET_LEN];
  TEST_ASSERT_EQUAL_UINT32(DATA_PACKET_LEN, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
  for (size_t i = 6; i < 14; ++i) TEST_ASSERT_EQUAL(0, buf[i]);
  PacketHeader h{}; PositionPayload decoded{};
  TEST_ASSERT_TRUE(protocol::decodeData(buf, sizeof(buf), h, decoded));
  TEST_ASSERT_FALSE(decoded.fix); TEST_ASSERT_FALSE(decoded.velocityValid);
  TEST_ASSERT_EQUAL_INT32(0, decoded.latE7);
  TEST_ASSERT_EQUAL_INT32(0, decoded.lonE7);
}

void test_strict_magic_version_type_length_and_id() {
  PacketHeader h{}; PositionPayload p{};
  uint8_t buf[32]{}; memcpy(buf, goldenData, sizeof(goldenData));
  for (size_t n = 0; n < DATA_PACKET_LEN; ++n)
    TEST_ASSERT_FALSE(protocol::decodeData(buf, n, h, p));
  for (size_t n = DATA_PACKET_LEN + 1; n <= sizeof(buf); ++n)
    TEST_ASSERT_FALSE(protocol::decodeData(buf, n, h, p));
  TEST_ASSERT_FALSE(protocol::decodeData(nullptr, DATA_PACKET_LEN, h, p));
  buf[0] ^= 1; TEST_ASSERT_FALSE(protocol::decodeHeader(buf, 18, h)); buf[0] ^= 1;
  buf[1] = 0x31; TEST_ASSERT_FALSE(protocol::decodeHeader(buf, 18, h));
  // Legacy v3 has an independent version byte and a 32-byte DATA format.
  buf[1] = 3; TEST_ASSERT_FALSE(protocol::decodeHeader(buf, 32, h));
  for (uint8_t type : {0, 3, 7, 15}) {
    buf[1] = 0x50 | type; TEST_ASSERT_FALSE(protocol::decodeHeader(buf, 18, h));
  }
  buf[1] = 0x51;
  for (uint16_t id : {uint16_t(0), uint16_t(0xFFFF)}) {
    protocol::putU16(buf + 2, id); TEST_ASSERT_FALSE(protocol::decodeHeader(buf, 18, h));
    auto header = dataHeader(); header.clientId = id;
    TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, sizeof(buf), header, position()));
  }
  protocol::putU16(buf + 2, 0x1234);
  TEST_ASSERT_TRUE(protocol::decodeHeader(buf, 18, h));
  TEST_ASSERT_EQUAL_HEX16(0x1234, h.clientId);  // binding filter belongs to caller
}

void test_bad_vectors_and_flags_reject_without_mutating_decoded_output() {
  uint8_t buf[DATA_PACKET_LEN]; memcpy(buf, goldenData, sizeof(buf));
  PacketHeader h{0x1234, 9, MSG_TELEMETRY}; PositionPayload out = position();
  for (uint16_t course : {uint16_t(3600), uint16_t(4094), uint16_t(4095)}) {
    protocol::putU16(buf + 15, 0xF000 | course);
    TEST_ASSERT_FALSE(protocol::decodeData(buf, sizeof(buf), h, out));
    TEST_ASSERT_EQUAL_HEX16(0x1234, h.clientId);
    TEST_ASSERT_EQUAL_INT32(241234560, out.latE7);
  }
  memcpy(buf, goldenData, sizeof(buf)); buf[14] = 255;
  TEST_ASSERT_FALSE(protocol::decodeData(buf, sizeof(buf), h, out));
  buf[14] = 2; TEST_ASSERT_FALSE(protocol::decodeData(buf, sizeof(buf), h, out));
  memcpy(buf, goldenData, sizeof(buf)); buf[16] &= ~0x40;
  TEST_ASSERT_FALSE(protocol::decodeData(buf, sizeof(buf), h, out));
  memcpy(buf, goldenData, sizeof(buf)); protocol::putI32(buf+6,900000001);
  TEST_ASSERT_FALSE(protocol::decodeData(buf, sizeof(buf), h, out));
  auto p = position(); p.satelliteClass = 4;
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
}

void test_invalid_vector_keeps_valid_position_and_unknown_course() {
  auto p = position(); p.velocityValid = false; p.speedDmS = 255; p.courseDeg10 = 4095;
  uint8_t buf[DATA_PACKET_LEN]; PacketHeader h{}; PositionPayload out{};
  TEST_ASSERT_EQUAL_UINT32(DATA_PACKET_LEN, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
  TEST_ASSERT_TRUE(protocol::decodeData(buf, sizeof(buf), h, out));
  TEST_ASSERT_TRUE(out.fix); TEST_ASSERT_FALSE(out.velocityValid);
  TEST_ASSERT_EQUAL(255, out.speedDmS); TEST_ASSERT_EQUAL(4095, out.courseDeg10);
  TEST_ASSERT_EQUAL_INT32(p.latE7, out.latE7);
  p.courseDeg10 = 3600;
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, sizeof(buf), dataHeader(), p));
}

void test_quantization_preserves_ranges_thresholds_and_unknowns() {
  TEST_ASSERT_EQUAL(0, protocol::quantizeSpeed(0));
  TEST_ASSERT_EQUAL(3, protocol::quantizeSpeed(0.29));  // caller gates raw low speed BEFORE rounding
  TEST_ASSERT_EQUAL(3, protocol::quantizeSpeed(0.30));
  TEST_ASSERT_EQUAL(4, protocol::quantizeSpeed(0.35));
  TEST_ASSERT_EQUAL(254, protocol::quantizeSpeed(25.4));
  for (double speed : {-0.01, 25.41, 25.45, 655.35, double(NAN), double(INFINITY)})
    TEST_ASSERT_EQUAL(255, protocol::quantizeSpeed(speed));
  TEST_ASSERT_EQUAL(15, protocol::quantizeHdopCenti(150));
  TEST_ASSERT_EQUAL(16, protocol::quantizeHdopCenti(151));
  TEST_ASSERT_EQUAL(30, protocol::quantizeHdopCenti(300));
  TEST_ASSERT_EQUAL(31, protocol::quantizeHdopCenti(301));
  TEST_ASSERT_EQUAL(254, protocol::quantizeHdopCenti(2540));
  TEST_ASSERT_EQUAL(255, protocol::quantizeHdopCenti(2541));
  TEST_ASSERT_EQUAL(255, protocol::quantizeHdopCenti(UINT32_MAX));
  TEST_ASSERT_EQUAL(15, protocol::quantizeHdop(1.5));
  TEST_ASSERT_EQUAL(16, protocol::quantizeHdop(1.501));
  TEST_ASSERT_EQUAL(31, protocol::quantizeHdop(3.04));
  for (double hdop : {-1.0, 25.41, double(NAN), double(INFINITY)})
    TEST_ASSERT_EQUAL(255, protocol::quantizeHdop(hdop));
}

void test_satellite_class_is_threshold_not_fake_exact_count() {
  TEST_ASSERT_EQUAL(0, protocol::satClass(-1));
  TEST_ASSERT_EQUAL(0, protocol::satClass(255));
  for (int n = 0; n <= 5; ++n) TEST_ASSERT_EQUAL(1, protocol::satClass(n));
  TEST_ASSERT_EQUAL(2, protocol::satClass(6)); TEST_ASSERT_EQUAL(2, protocol::satClass(7));
  TEST_ASSERT_EQUAL(3, protocol::satClass(8)); TEST_ASSERT_EQUAL(3, protocol::satClass(254));
  TEST_ASSERT_EQUAL(-1, protocol::satLowerBound(0)); TEST_ASSERT_EQUAL(0, protocol::satLowerBound(1));
  TEST_ASSERT_EQUAL(6, protocol::satLowerBound(2)); TEST_ASSERT_EQUAL(8, protocol::satLowerBound(3));
  TEST_ASSERT_EQUAL(-1, protocol::satLowerBound(255));
}

void test_telemetry_golden_exact_satellite_diagnostics_and_unknowns() {
  const PacketHeader header{0xAB12, 0x43, MSG_TELEMETRY};
  TelemetryPayload telemetry{4150, -5, 87, 9};
  const uint8_t golden[] = {0x53, 0x54, 0x12, 0xAB, 0x43, 0, 0x36, 0x10, 0xFB, 87, 9};
  uint8_t buf[TELEMETRY_PACKET_LEN]; PacketHeader h{}; TelemetryPayload out{};
  TEST_ASSERT_EQUAL_UINT32(sizeof(golden), protocol::encodeTelemetry(buf, sizeof(buf), header, telemetry));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, buf, sizeof(golden));
  TEST_ASSERT_TRUE(protocol::decodeTelemetry(buf, sizeof(buf), h, out));
  TEST_ASSERT_EQUAL(4150, out.batteryMv); TEST_ASSERT_EQUAL(-5, out.tempC);
  TEST_ASSERT_EQUAL(87, out.humidityPct); TEST_ASSERT_EQUAL(9, out.satellites);
  telemetry = {0, INT8_MIN, 255, 255};
  protocol::encodeTelemetry(buf, sizeof(buf), header, telemetry);
  TEST_ASSERT_TRUE(protocol::decodeTelemetry(buf, sizeof(buf), h, out));
  TEST_ASSERT_EQUAL(INT8_MIN, out.tempC); TEST_ASSERT_EQUAL(255, out.satellites);
  buf[9] = 101; TEST_ASSERT_FALSE(protocol::decodeTelemetry(buf, sizeof(buf), h, out));
  telemetry.humidityPct = 254;
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeTelemetry(buf, sizeof(buf), header, telemetry));
}

void test_all_codecs_reject_wrong_type_short_capacity_and_extra_bytes() {
  uint8_t buf[32]{}; PacketHeader h{}; PositionPayload p{}; TelemetryPayload t{};
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(nullptr, 17, dataHeader(), position()));
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, 16, dataHeader(), position()));
  auto wrong = dataHeader(); wrong.msgType = 2; // retired ACK
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeData(buf, 17, wrong, position()));
  const uint8_t legacyAck[11] = {0x53, 0x42, 0x34, 0x12, 8, 0, 8, 0, 0, 0, 0};
  TEST_ASSERT_FALSE(protocol::decodeHeader(legacyAck, sizeof(legacyAck), h));
  TEST_ASSERT_FALSE(protocol::decodeData(legacyAck, sizeof(legacyAck), h, p));
  TEST_ASSERT_FALSE(protocol::decodeTelemetry(legacyAck, sizeof(legacyAck), h, t));
  const PacketHeader th{0x1234, 0, MSG_TELEMETRY};
  const TelemetryPayload tp{4000, 20, 50, 8};
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeTelemetry(buf, 10, th, tp));
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeTelemetry(buf, 11, wrong, tp));
  protocol::encodeTelemetry(buf, sizeof(buf), th, tp);
  for (size_t n = 0; n <= sizeof(buf); ++n) if (n != TELEMETRY_PACKET_LEN)
    TEST_ASSERT_FALSE(protocol::decodeTelemetry(buf, n, h, t));
}

void test_data_sequence_wrap_remains_independent_of_telemetry() {
  uint8_t buf[MAX_PACKET_LEN]; PacketHeader h{}; PositionPayload p{};
  command_freshness::RadioSequence gate;
  uint32_t now = 1000;
  for (uint16_t seq : {uint16_t(65534), uint16_t(65535), uint16_t(0), uint16_t(1)}) {
    protocol::encodeData(buf, sizeof(buf), dataHeader(seq), position());
    TEST_ASSERT_TRUE(protocol::decodeData(buf, DATA_PACKET_LEN, h, p));
    TEST_ASSERT_EQUAL_UINT16(seq, h.seq); TEST_ASSERT_TRUE(gate.accept(h.seq, now));
    TEST_ASSERT_FALSE(gate.accept(h.seq, now + 1));
    const PacketHeader telHeader{h.clientId, 9876, MSG_TELEMETRY};
    const TelemetryPayload tel{4000, 20, 40, 8};
    protocol::encodeTelemetry(buf, sizeof(buf), telHeader, tel);
    // Telemetry has its own counter and is never submitted to the DATA gate.
    now += 500;
  }
}

void test_diagnostic_golden_counter_endpoints_reserved_bits_and_exact_length() {
  const PacketHeader header{0xAB12, 0x44, MSG_DIAGNOSTIC};
  DiagnosticPayload diagnostics{500, 0x1234, 0x8000, 0xFFFF, 0x0102, 0x3F};
  const uint8_t golden[] = {0x53, 0x55, 0x12, 0xAB, 0x44, 0,
      0xF4, 1, 0x34, 0x12, 0, 0x80, 0xFF, 0xFF, 2, 1, 0x3F};
  uint8_t buf[32]{}; PacketHeader h{}; DiagnosticPayload out{};
  TEST_ASSERT_EQUAL_UINT32(17, protocol::encodeDiagnostic(buf, sizeof(buf), header, diagnostics));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, buf, sizeof(golden));
  TEST_ASSERT_TRUE(protocol::decodeDiagnostic(buf, DIAGNOSTIC_PACKET_LEN, h, out));
  TEST_ASSERT_EQUAL(500, out.epochIntervalMs); TEST_ASSERT_EQUAL_HEX16(0x1234, out.backlogDrops);
  TEST_ASSERT_EQUAL_HEX16(0x8000, out.nmeaErrors); TEST_ASSERT_EQUAL_HEX16(0xFFFF, out.txErrors);
  TEST_ASSERT_EQUAL_HEX16(0x0102, out.skippedSlots); TEST_ASSERT_EQUAL_HEX8(0x3F, out.status);
  for (size_t n = 0; n <= sizeof(buf); ++n) if (n != DIAGNOSTIC_PACKET_LEN)
    TEST_ASSERT_FALSE(protocol::decodeDiagnostic(buf, n, h, out));
  PositionPayload p{};
  TEST_ASSERT_FALSE(protocol::decodeData(buf, DIAGNOSTIC_PACKET_LEN, h, p));
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeDiagnostic(buf, 16, header, diagnostics));
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeDiagnostic(nullptr, 17, header, diagnostics));
  TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeDiagnostic(buf, 17, dataHeader(), diagnostics));
  for (uint8_t status : {uint8_t(0x80), uint8_t(0xBF)}) {
    buf[16] = status; TEST_ASSERT_FALSE(protocol::decodeDiagnostic(buf, 17, h, out));
    diagnostics.status = status;
    TEST_ASSERT_EQUAL_UINT32(0, protocol::encodeDiagnostic(buf, 17, header, diagnostics));
  }
  for (uint8_t status : {uint8_t(0x40), uint8_t(0xFF)}) {
    diagnostics.status=status;
    TEST_ASSERT_EQUAL_UINT32(17, protocol::encodeDiagnostic(buf,17,header,diagnostics));
    TEST_ASSERT_TRUE(protocol::decodeDiagnostic(buf,17,h,out));
    TEST_ASSERT_EQUAL_UINT8(status,out.status);
  }
  diagnostics = {0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT32(17, protocol::encodeDiagnostic(buf, 17, header, diagnostics));
  TEST_ASSERT_TRUE(protocol::decodeDiagnostic(buf, 17, h, out));
  TEST_ASSERT_EQUAL(0, out.status); TEST_ASSERT_EQUAL(0, out.epochIntervalMs);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_data_golden_bytes_define_wire_independent_of_struct_layout);
  RUN_TEST(test_signed32_world_endpoints_round_trip);
  RUN_TEST(test_coordinate_overflow_rejects_valid_fix_without_writing);
  RUN_TEST(test_no_fix_invalid_coordinates_are_canonical_not_wrapped);
  RUN_TEST(test_strict_magic_version_type_length_and_id);
  RUN_TEST(test_bad_vectors_and_flags_reject_without_mutating_decoded_output);
  RUN_TEST(test_invalid_vector_keeps_valid_position_and_unknown_course);
  RUN_TEST(test_quantization_preserves_ranges_thresholds_and_unknowns);
  RUN_TEST(test_satellite_class_is_threshold_not_fake_exact_count);
  RUN_TEST(test_telemetry_golden_exact_satellite_diagnostics_and_unknowns);
  RUN_TEST(test_all_codecs_reject_wrong_type_short_capacity_and_extra_bytes);
  RUN_TEST(test_data_sequence_wrap_remains_independent_of_telemetry);
  RUN_TEST(test_diagnostic_golden_counter_endpoints_reserved_bits_and_exact_length);
  return UNITY_END();
}
