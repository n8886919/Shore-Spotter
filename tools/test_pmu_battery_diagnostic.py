#!/usr/bin/env python3
"""Fault-inject actual Client PMU diagnostic functions without a serial device.

The fake Wire bus permits only register-address writes followed by reads. JSON
is decoded by Python and each emitted record passes the real Flash frame codec.
This tests observation behavior, not battery health or physical power delivery.
"""
from pathlib import Path
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/main.cpp").read_text()
XP = ROOT / ".pio/libdeps/tbeam-client-trip/XPowersLib/src"
if not XP.exists():
    XP = ROOT / ".pio/libdeps/tbeam-client-diagnostic/XPowersLib/src"


def block(marker):
    start = SOURCE.index(marker)
    body = SOURCE.index("{", start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, SOURCE[body:]):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if not depth:
                return SOURCE[start:body + match.end()]
    raise ValueError(marker)


CPP = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "REG/AXP2101Constants.h"
#include "diagnostic_store_codec.h"
#define ROLE_CLIENT 1
#define FIELD_DIAGNOSTIC 1
#define F(x) x
static uint32_t nowMs=1000, powerBootId=12345;
static uint16_t cachedBatteryMv=4159;
static bool pmuOnline=true;
static int pmuBatteryInit[4]={-1,-1,-1,-1};
static constexpr int PMU_SDA_PIN=42, PMU_SCL_PIN=41, I2C_TRANSACTION_TIMEOUT_MS=10;
static unsigned guardTries=0, guardDepth=0;
static bool lockAvailable=true, enforceGuard=true;
static uint32_t stepMs=0;
static uint32_t millis() { return nowMs; }
struct ClientRailGuard {
  bool held;
  explicit ClientRailGuard(bool wait=false):held(lockAvailable) {
    assert(!wait); ++guardTries;
    if (held) { assert(guardDepth==0); ++guardDepth; }
  }
  ~ClientRailGuard() { if (held) { assert(guardDepth==1); --guardDepth; } }
};
struct Fault {
  size_t writeCount=1, requested=1;
  uint8_t txStatus=0;
  int available=1, readOverride=-999;
};
static std::array<uint8_t,256> registers;
static std::array<Fault,256> faults;
static std::vector<unsigned> addressWrites, requests, valueReads;
static std::vector<std::string> transport, pmuCalls, logs, records;
static bool submitAccept=true;
static size_t longestRecord=0;
static uint32_t lastSubmitMs=0;
static unsigned completedTransactions=0;
struct FakeWire {
  int selected=-1;
  bool inTransaction=false;
  unsigned writesThisTransaction=0;
  void begin(int sda,int scl) { assert(sda==42 && scl==41); }
  void setTimeOut(uint16_t ms) { assert(ms==10); }
  void beginTransmission(uint8_t address) {
    assert(address==AXP2101_SLAVE_ADDRESS && !inTransaction);
    if(enforceGuard) assert(guardDepth==1);
    inTransaction=true; selected=-1; writesThisTransaction=0;
    transport.push_back("begin"); nowMs+=stepMs;
  }
  size_t write(uint8_t reg) {
    assert(inTransaction && writesThisTransaction++==0);
    // A second byte would change a PMU register and must fail this fixture.
    selected=reg; addressWrites.push_back(reg); transport.push_back("address");
    return faults[reg].writeCount;
  }
  size_t write(const uint8_t*,size_t)=delete;
  uint8_t endTransmission() {
    assert(inTransaction && writesThisTransaction==1 && selected>=0);
    inTransaction=false; ++completedTransactions;
    transport.push_back("end"); nowMs+=stepMs;
    return faults[selected].txStatus;
  }
  size_t requestFrom(uint8_t address,uint8_t length) {
    assert(address==AXP2101_SLAVE_ADDRESS && length==1 && !inTransaction);
    assert(selected>=0); requests.push_back(selected); transport.push_back("request");
    nowMs+=stepMs; return faults[selected].requested;
  }
  int available() {
    assert(selected>=0 && !inTransaction); transport.push_back("available");
    return faults[selected].available;
  }
  int read() {
    assert(selected>=0 && !inTransaction); transport.push_back("read");
    valueReads.push_back(selected); nowMs+=stepMs;
    int result=faults[selected].readOverride;
    return result==-999 ? registers[selected] : result;
  }
  // Inherited Stream::readBytes could wait one second after a short I2C read.
  size_t readBytes(uint8_t*,size_t)=delete;
  size_t readBytes(char*,size_t)=delete;
} PMUWire;
struct FakePmu {
  bool beginOk=true;
  bool initResult[4]={true,true,true,true};
  bool begin(FakeWire&,uint8_t address,int sda,int scl) {
    assert(address==AXP2101_SLAVE_ADDRESS && sda==42 && scl==41);
    pmuCalls.push_back("begin"); return beginOk;
  }
  bool enableBattDetection(){pmuCalls.push_back("detect");return initResult[0];}
  bool enableVbusVoltageMeasure(){pmuCalls.push_back("vbus");return initResult[1];}
  bool enableBattVoltageMeasure(){pmuCalls.push_back("battery");return initResult[2];}
  bool enableSystemVoltageMeasure(){pmuCalls.push_back("system");return initResult[3];}
  bool setALDO4Voltage(int mv){assert(mv==3300);pmuCalls.push_back("aldo4_mv");return true;}
  bool enableALDO4(){pmuCalls.push_back("aldo4_on");return true;}
  int readRegister(uint8_t)=delete;
  int writeRegister(uint8_t,uint8_t)=delete;
  void shutdown()=delete;
} pmu;
struct FakeLog {
  void println(const char* text) { assert(guardDepth==0); logs.emplace_back(text); }
} Log;
namespace diagnostic_store {
bool submit(uint16_t kind,const void* data,size_t length,uint32_t ms) {
  assert(guardDepth==0); // storage/console work must release the shared rail lock
  assert(kind==2 && length<320 && length<=codec::kMaxRecordBytes);
  std::string line(static_cast<const char*>(data),length);
  assert(line.front()=='{' && line.back()=='}');
  uint8_t frame[codec::kFrameBytes];
  codec::beginFrame(frame,powerBootId,0,ms);
  assert(codec::append(frame,kind,data,length,ms));
  codec::seal(frame); assert(codec::validFrame(frame));
  assert(!std::memcmp(frame+codec::kHeaderBytes+codec::kEnvelopeBytes,data,length));
  longestRecord=std::max(longestRecord,length);
  records.push_back(line); lastSubmitMs=ms;
  return submitAccept;
}
}
'''
for marker in (
    "static int readPmuBatteryDiagnosticRegister(uint8_t reg)",
    "static void recordClientBatteryDiagnostic(const char *stage)",
    "static bool initPmu()",
):
    CPP += block(marker) + "\n"

CPP += r'''
static const std::array<unsigned,8> expectedOrder={0x00,0x01,0x30,0x34,0x35,0x68,0x22,0x27};
void resetFixture() {
  assert(guardDepth==0 && !PMUWire.inTransaction);
  nowMs=1000; powerBootId=12345; cachedBatteryMv=4159;
  pmuOnline=true; lockAvailable=true; enforceGuard=true; stepMs=0;
  guardTries=0; completedTransactions=0; submitAccept=true; pmu=FakePmu{};
  for(int& v:pmuBatteryInit) v=-1;
  for(unsigned i=0;i<256;++i) { registers[i]=uint8_t(i); faults[i]=Fault{}; }
  registers[0]=24; registers[1]=93; registers[0x30]=13;
  registers[0x34]=0x10; registers[0x35]=0x40; registers[0x68]=1;
  registers[0x22]=2; registers[0x27]=0x1b;
  transport.clear(); addressWrites.clear(); requests.clear(); valueReads.clear();
  pmuCalls.clear(); logs.clear(); records.clear();
}
void printIntArray(const std::array<int,8>& values) {
  std::printf("[%d,%d,%d,%d,%d,%d,%d,%d]",values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7]);
}
void checkRecord(const char* name,const char* stage,const std::array<int,8>& expectedRaw,
                 unsigned failureMask) {
  const auto savedRegisters=registers;
  const uint16_t savedCache=cachedBatteryMv;
  const uint32_t start=nowMs, savedBoot=powerBootId;
  int savedInit[4]; std::memcpy(savedInit,pmuBatteryInit,sizeof(savedInit));
  const bool online=pmuOnline, lock=lockAvailable;
  const size_t pmuCallCount=pmuCalls.size();
  recordClientBatteryDiagnostic(stage);
  assert(guardDepth==0 && !PMUWire.inTransaction);
  assert(registers==savedRegisters && cachedBatteryMv==savedCache && powerBootId==savedBoot);
  assert(!std::memcmp(pmuBatteryInit,savedInit,sizeof(savedInit)));
  assert(pmuCalls.size()==pmuCallCount); // diagnostic never calls a PMU setter/helper
  assert(records.size()==1 && logs.size()==1 && records[0]==logs[0]);
  assert(lastSubmitMs==start);
  if(online && lock) {
    assert(guardTries==1);
    assert(addressWrites==std::vector<unsigned>(expectedOrder.begin(),expectedOrder.end()));
    assert(completedTransactions==8);
  } else {
    assert(addressWrites.empty() && requests.empty() && valueReads.empty());
    assert(completedTransactions==0 && guardTries==1);
  }
  std::printf("EVIDENCE {\"case\":\"%s\",\"expected\":{\"stage\":\"%s\",\"boot_id\":%lu,\"ms\":%lu,"
              "\"online\":%s,\"lock\":%s,\"init\":[%d,%d,%d,%d],\"read_fail\":%u,\"elapsed_ms\":%lu,\"raw\":",
      name,stage,(unsigned long)savedBoot,(unsigned long)start,online?"true":"false",lock?"true":"false",
      savedInit[0],savedInit[1],savedInit[2],savedInit[3],failureMask,(unsigned long)(nowMs-start));
  printIntArray(expectedRaw);
  std::printf("},\"actual\":%s}\n",records[0].c_str());
}
void checkTransport(const char* name,Fault fault,int expected,
                    const std::vector<std::string>& expectedTrace) {
  resetFixture(); faults[0x35]=fault;
  int result;
  { ClientRailGuard guard; result=readPmuBatteryDiagnosticRegister(0x35); }
  assert(result==expected && transport==expectedTrace && completedTransactions==1);
  assert(!PMUWire.inTransaction && guardDepth==0);
  std::printf("PASS %s\n",name);
}
int main() {
  const std::array<int,8> healthy={24,93,13,0x10,0x40,1,2,0x1b};
  resetFixture(); checkRecord("battery_present","boot",healthy,0);
  resetFixture(); registers[0]=32; registers[1]=20; registers[0x34]=registers[0x35]=0;
  checkRecord("battery_absent_is_not_transport_failure","periodic",{32,20,13,0,0,1,2,27},0);
  resetFixture(); registers[0x30]=0; registers[0x68]=0;
  checkRecord("disabled_detection_and_adc_are_observed_without_reenable","periodic",{24,93,0,16,64,0,2,27},0);
  resetFixture(); registers[0x34]=0xf0; registers[0x35]=255;
  checkRecord("full_vbat_high_byte_debug_bits_retained","periodic",{24,93,13,240,255,1,2,27},0);

  Fault fault;
  fault.writeCount=0;
  checkTransport("failed address enqueue still completes transmission and does not request data",fault,-100,{"begin","address","end"});
  fault=Fault{}; fault.writeCount=0; fault.txStatus=4;
  checkTransport("address enqueue failure has its own code even when endTransmission also fails",fault,-100,{"begin","address","end"});
  fault=Fault{}; fault.writeCount=2;
  checkTransport("unexpected address write count is also rejected",fault,-100,{"begin","address","end"});
  for(uint8_t status:{uint8_t(1),uint8_t(2),uint8_t(3),uint8_t(4),uint8_t(5),uint8_t(255)}) {
    fault=Fault{}; fault.txStatus=status;
    checkTransport("endTransmission failure preserves negative transport status",fault,-100-int(status),{"begin","address","end"});
  }
  fault=Fault{}; fault.requested=0;
  checkTransport("short request rejects data immediately without Stream readBytes",fault,-200,{"begin","address","end","request"});
  fault=Fault{}; fault.requested=2;
  checkTransport("unexpected oversized request is not accepted",fault,-200,{"begin","address","end","request"});
  fault=Fault{}; fault.available=0;
  checkTransport("empty buffer after successful request never calls read",fault,-201,{"begin","address","end","request","available"});
  for(int invalid:{-1,256}) {
    fault=Fault{}; fault.readOverride=invalid;
    checkTransport("out of range read result preserves a diagnostic failure",fault,-202,{"begin","address","end","request","available","read"});
  }
  fault=Fault{};
  checkTransport("healthy transport returns exact register byte",fault,0x40,{"begin","address","end","request","available","read"});

  for(unsigned i=0;i<expectedOrder.size();++i) {
    resetFixture(); faults[expectedOrder[i]].txStatus=2;
    auto expected=healthy; expected[i]=-102;
    std::string name="failed_register_index_"+std::to_string(i);
    checkRecord(name.c_str(),"periodic",expected,1U<<i);
  }
  resetFixture(); faults[0x35].requested=0;
  checkRecord("vbat_low_failure_keeps_successful_high_byte","periodic",{24,93,13,16,-200,1,2,27},16);
  resetFixture(); faults[0].writeCount=0; faults[1].txStatus=4;
  faults[0x30].requested=0; faults[0x34].available=0;
  faults[0x35].readOverride=-1; faults[0x68].readOverride=256;
  faults[0x22].txStatus=1; faults[0x27].txStatus=5;
  checkRecord("all_eight_transport_failures_remain_distinct","periodic",{-100,-104,-200,-201,-202,-202,-101,-105},255);
  resetFixture(); pmuOnline=false;
  checkRecord("offline_no_bus_access","boot",{-2,-2,-2,-2,-2,-2,-2,-2},0);
  resetFixture(); lockAvailable=false;
  checkRecord("busy_guard_no_bus_access","periodic",{-2,-2,-2,-2,-2,-2,-2,-2},0);
  resetFixture(); submitAccept=false;
  checkRecord("full_flash_queue_still_emits_console_evidence_without_retry","periodic",healthy,0);
  resetFixture(); nowMs=UINT32_MAX-3; stepMs=1;
  checkRecord("elapsed_time_wrap_is_unsigned","periodic",healthy,0);
  resetFixture(); nowMs=powerBootId=UINT32_MAX; stepMs=UINT32_MAX/16;
  for(unsigned reg:expectedOrder) faults[reg].txStatus=255;
  checkRecord("max_width_identifiers_and_errors_complete_json","periodic",{-355,-355,-355,-355,-355,-355,-355,-355},255);

  // Extracted real initialization retains exactly one call to every existing
  // setter. Failed settings must be reported, not retried or treated as no PMU.
  for(unsigned mask=0;mask<16;++mask) {
    resetFixture(); enforceGuard=false;
    for(unsigned i=0;i<4;++i) pmu.initResult[i]=bool(mask&(1U<<i));
    assert(initPmu());
    assert(pmuCalls==std::vector<std::string>({"begin","detect","vbus","battery","system","aldo4_mv","aldo4_on"}));
    for(unsigned i=0;i<4;++i) assert(pmuBatteryInit[i]==int(bool(mask&(1U<<i))));
  }
  resetFixture(); enforceGuard=false; pmu.beginOk=false;
  assert(!initPmu() && pmuCalls==std::vector<std::string>({"begin"}));
  for(int value:pmuBatteryInit) assert(value==-1);
  std::puts("PASS init result capture: all 16 boolean combinations, original setters once each, failed PMU leaves init unattempted");
  std::printf("PASS all diagnostic records preserve raw state, cache and IRQs; real Flash codec round trip; max JSON %zu/319 bytes\n",longestRecord);
}
'''

with tempfile.TemporaryDirectory(prefix="shore-pmu-battery-regression-") as directory:
    source = Path(directory) / "probe.cpp"
    binary = Path(directory) / "probe"
    source.write_text(CPP)
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT / "include"),
        "-I", str(XP), str(source), "-o", str(binary),
    ], check=True)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    cases = 0
    for line in result.stdout.splitlines():
        if line.startswith("EVIDENCE "):
            evidence = json.loads(line.removeprefix("EVIDENCE "))
            expected, actual = evidence["expected"], evidence["actual"]
            assert actual["event"] == "pmu_battery", evidence
            for key, value in expected.items():
                assert actual[key] == value, (evidence["case"], key, actual, expected)
            assert isinstance(actual["online"], bool) and isinstance(actual["lock"], bool), actual
            cases += 1
            print("PASS", evidence["case"])
        else:
            print(line)
    assert cases == 19, cases
    print(f"PASS {cases} real-handler diagnostic JSON cases")
