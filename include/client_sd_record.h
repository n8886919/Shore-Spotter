#pragma once
#include "gnss_snapshot.h"
#include "protocol.h"
namespace sd_log {
enum class ClientKind : uint8_t { Snapshot, Resume, Pause, TxStarted, TxSent, TxFailed, TxTimeout, TxCancelled };
struct ClientRecord {
  ClientKind kind = ClientKind::Snapshot;
  uint32_t ms = 0, txStartedMs = 0;
  uint16_t nodeId = 0, batteryMv = 0;
  gnss_snapshot::Snapshot gps;
  gnss_snapshot::Counters gnss;
  uint32_t byteAgeMs = UINT32_MAX, sentenceAgeMs = UINT32_MAX;
  uint32_t backlogDrops = 0, txCount = 0, txErrors = 0, skipped = 0, radioRestarts = 0;
  uint32_t loopGapMaxMs = 0, loopOver250 = 0, heapFree = 0;
  float epochHz = 0, rmcHz = 0, ggaHz = 0;
  bool recovering = false;
  int16_t txStatus = 0;
  uint8_t length = 0, raw[MAX_PACKET_LEN]{};
};
static_assert(sizeof(ClientRecord) < 384, "Bound main-loop value copies");
size_t encodeClient(char *out, size_t capacity, uint32_t boot, const ClientRecord &record);
}
