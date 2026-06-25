#pragma once
// ---------------------------------------------------------------------------
// Shore Spotter — shared LoRa wire protocol.
//
// 這些宣告是 CLIENT 與 SERVER 兩端共用的「封包格式契約」。
// 任何欄位/常數修改都會同時影響上下行，集中於此可避免兩端不同步。
//
// 對應文件：docs/interface.md
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>

struct __attribute__((packed)) PacketHeader {
  uint8_t magic;
  uint8_t version;
  uint8_t networkId;
  uint16_t srcId;
  uint16_t dstId;
  uint8_t msgType;
  uint16_t seq;
  uint8_t payloadLen;
};

struct __attribute__((packed)) PositionPayload {
  int32_t  latE7;        // latitude  * 1e7 (fixed-point, exact and compact)
  int32_t  lonE7;        // longitude * 1e7
  uint8_t  fix;          // GPS fix (0/1)
  uint16_t speedCmS;     // ground speed, cm/s
  uint16_t courseDeg10;  // track angle 0.1° units (0–3599)
  int16_t  accelCmS2;    // smoothed acceleration, cm/s²
};

// Battery + environment sent separately every 10 s to reduce 1 Hz packet size.
struct __attribute__((packed)) TelemetryPayload {
  uint16_t batteryMv;    // battery voltage mV (0 = unknown)
  int16_t  tempC10;      // temperature × 10 (0.1 °C resolution, INT16_MIN = no sensor)
  uint8_t  humidityPct;  // relative humidity 0–100 %, 0xFF = no sensor
};

struct __attribute__((packed)) AckPayload {
  uint16_t ackSeq;
  int16_t rssiDbm10;
  int8_t snrDb10;
};

// ---------------------------------------------------------------------------
// Application protocol (stage 1: binary header, ready for 1->many / many->many)
// ---------------------------------------------------------------------------
// Grouping works on two layers:
//   * RF_SYNC_WORD (PHY, defined in main.cpp) = coarse filter, cheap power saving.
//   * NETWORK_ID (below)                       = logical group, authoritative.
constexpr uint8_t PROTO_MAGIC = 0x53;
constexpr uint8_t PROTO_VERSION = 1;
constexpr uint16_t ID_BROADCAST = 0xFFFF;
constexpr uint8_t MAC_LEN = 4;  // reserved for HMAC (stage 2), zero-filled now

constexpr uint8_t NETWORK_ID = 0x01;  // group id; different groups => different value
constexpr uint16_t SERVER_ID = 0x0010;   // shore station id
// nodeId is derived at runtime from the ESP32 chip MAC (last 2 bytes).
// No need to change source code per board — each chip is already unique.

enum MsgType : uint8_t {
  MSG_DATA      = 1,
  MSG_ACK       = 2,
  MSG_HELLO     = 3,
  MSG_TELEMETRY = 4,  // battery + temp/humidity, 30 s interval
};

constexpr size_t DATA_PACKET_LEN =
    sizeof(PacketHeader) + sizeof(PositionPayload) + MAC_LEN;
constexpr size_t TELEMETRY_PACKET_LEN =
    sizeof(PacketHeader) + sizeof(TelemetryPayload) + MAC_LEN;
constexpr size_t ACK_PACKET_LEN =
  sizeof(PacketHeader) + sizeof(AckPayload) + MAC_LEN;
