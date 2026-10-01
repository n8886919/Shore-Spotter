"""Run actual radio initialization/recovery with a host radio stub.

Run: python3 tools/test_radio_power.py
Checks configured power and failure propagation, not measured RF output/range.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("SHORE_RADIO_POWER_SOURCE", ROOT / "src/main.cpp")).read_text()


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
    raise ValueError(f"Unterminated source block: {marker}")


# Legacy persisted values must not override fixed power anywhere in the Client
# boot/loop/recovery path. Keep this structural check small; no Preferences mock.
code = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', '', SOURCE)
assert not re.search(
    r'\b(?:loadClientSettings|saveClientSettings|applyTxPower|evaluateAtpc)\s*\(', code
), "Client still calls/defines an adaptive or persisted power path"
assert not re.search(r'\bget(?:Char|Bool)\s*\(\s*"(?:txpwr|atpc)"', code), \
    "Client still reads legacy power settings"
assert not re.search(r'\bradio\.setOutputPower\s*\(', code), \
    "Power must remain owned by radio initialization"

cpp = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#define F(text) text
constexpr int RADIOLIB_ERR_NONE = 0;
constexpr int LORA_SCK = 1, LORA_MISO = 2, LORA_MOSI = 3, LORA_NSS = 4;
uint32_t clockMs = 1;
uint32_t millis() { return clockMs; }
void delay(uint32_t ms) { clockMs += ms; }
void vTaskDelay(uint32_t ms) { delay(ms); }
#define pdMS_TO_TICKS(ms) (ms)
constexpr int PMU_SDA_PIN=42, PMU_SCL_PIN=41, AXP2101_SLAVE_ADDRESS=0x34, I2C_TRANSACTION_TIMEOUT_MS=10;
struct WireStub { void begin(int,int) {} void setTimeOut(int) {} } PMUWire;
struct PmuStub {
  bool beginOk=true, setOk=true, enableOk=true, readEnabled=true;
  int readVoltage=3300, voltage=0, sets=0, enables=0;
  bool enabled=false;
  bool begin(WireStub&,int,int,int) { return beginOk; }
  void enableBattDetection() {} void enableVbusVoltageMeasure() {}
  void enableBattVoltageMeasure() {} void enableSystemVoltageMeasure() {}
  void setALDO4Voltage(int) {} void enableALDO4() {}
  bool setALDO3Voltage(int v) { ++sets; voltage=v; return setOk; }
  bool enableALDO3() { ++enables; enabled=enableOk; return enableOk; }
  bool isEnableALDO3() { return readEnabled && enabled; }
  int getALDO3Voltage() { return readVoltage; }
} pmu;
bool pmuOnline = false;

struct LogStub {
  std::ostringstream text;
  template<class T> void print(T value) { text << value; }
  template<class T> void println(T value) { text << value << '\n'; }
  void clear() { text.str(""); text.clear(); }
  bool contains(const char *value) const { return text.str().find(value) != std::string::npos; }
} Log;
struct SpiStub { void begin(int, int, int, int) { assert(pmuOnline && pmu.enabled && pmu.voltage==3300); } } SPI;
struct RadioStub {
  int beginResult = 0, rxResult = 0, boostResult = 0;
  unsigned boostCalls=0;
  int setRxBoostedGainMode(bool enabled) { assert(enabled); ++boostCalls; return boostResult; }
  unsigned rxCalls = 0, standbyCalls = 0;
  std::vector<int> powers;
  void (*irq)() = nullptr;
  int begin(float, float, int, int, int, int power) {
    powers.push_back(power); return beginResult;
  }
  void setDio1Action(void (*callback)()) { irq = callback; }
  int startReceive() { ++rxCalls; return rxResult; }
  int standby() { ++standbyCalls; return rxResult; }
} radio;
void onClientDio1() {}
void onLoRaDio1() {}
bool clientTxFlag = true, stationRadioIrq = true;
'''
# Preserve the actual role preprocessor branch, instead of copying either power
# value into the implementation under test. Build this fixture once per role.
cpp += SOURCE[SOURCE.index("constexpr float RF_FREQUENCY"):
              SOURCE.index("constexpr uint32_t SEND_INTERVAL_MS")]
cpp += re.search(r'constexpr[^;\n]*\bRADIO_RECOVER_MIN_MS\b[^;]*;', SOURCE).group() + "\n"
cpp += r'''
uint16_t radioFailStreak = 0;
uint32_t lastRadioRecoverMs = 0, radioRecoverCount = 0;
'''
cpp += block("static bool initPmu(") + "\n"
cpp += "std::atomic_flag clientRailLock = ATOMIC_FLAG_INIT;\n"
cpp += block("struct ClientRailGuard {") + ";\n"
cpp += block("static bool prepareRadioPower(") + "\n"
cpp += block("static bool initRadio(") + "\n"
cpp += block("static bool recoverRadio(") + "\n"
cpp += r'''
void reset() {
  radio = RadioStub{}; Log.clear(); clockMs = 1;
  pmu=PmuStub{}; pmuOnline=initPmu(); Log.clear();
  radioFailStreak = 5; lastRadioRecoverMs = 0; radioRecoverCount = 0;
  clientTxFlag = stationRadioIrq = true;
}
void expectPower() {
  assert(!radio.powers.empty());
  for (const int power : radio.powers) assert(power == EXPECTED_DBM);
}
int main() {
#if defined(ROLE_CLIENT)
  reset(); clientRailLock.test_and_set();
  const auto before=clockMs; assert(!initRadio()&&clockMs==before&&pmu.sets==0);
  clientRailLock.clear(); assert(initRadio());
#endif
  for (int failure=0; failure<5; ++failure) {
    reset();
    if (failure==0) pmuOnline=false;
    if (failure==1) pmu.setOk=false;
    if (failure==2) pmu.enableOk=false;
    if (failure==3) pmu.readEnabled=false;
    if (failure==4) pmu.readVoltage=3200;
    assert(!initRadio() && radio.powers.empty());
    assert(!recoverRadio() && radio.powers.empty());
    assert(Log.contains("ALDO3") && !Log.contains("radio re-init ok"));
  }
  reset(); pmu.beginOk=false; pmuOnline=initPmu();
  assert(!pmuOnline && !initRadio() && radio.powers.empty());
  // Boot and successful reinitialization must use the same per-role power.
  reset(); assert(initRadio()); expectPower();
  assert(recoverRadio()); expectPower();
  assert(radio.powers.size() == 2 && radio.rxCalls + radio.standbyCalls == 1);
  assert(radioRecoverCount == 1 && radioFailStreak == 0);
  assert(pmu.sets==2 && pmu.enables==2 && pmu.voltage==3300);
  assert(Log.contains("radio re-init ok"));
#if defined(ROLE_CLIENT)
  assert(radio.irq == onClientDio1 && !clientTxFlag && radio.rxCalls == 0);
#else
  assert(radio.irq == onLoRaDio1 && !stationRadioIrq);
#endif

#if defined(ROLE_STATION)
  assert(radio.boostCalls == 2);
  reset(); radio.boostResult=-7; assert(!initRadio());
  assert(Log.contains("boosted gain failed") && !Log.contains("init ok"));
#endif
  reset(); radio.beginResult = -5;
  assert(!initRadio()); expectPower();
  assert(!Log.contains("init ok") && radio.rxCalls == 0);

  // Failed recovery initialization must not claim success or arm RX; a later
  // retry must still use the fixed role power rather than an old default.
  reset(); radio.beginResult = -5;
  assert(!recoverRadio()); expectPower();
  assert(radio.rxCalls == 0 && radioFailStreak == 5);
  assert(!Log.contains("radio re-init ok"));
  radio.beginResult = 0; clockMs += RADIO_RECOVER_MIN_MS;
  assert(recoverRadio()); expectPower();
  assert(radio.powers.size() == 2 && radio.rxCalls + radio.standbyCalls == 1);

  // Reinitialization alone is insufficient if continuous RX cannot restart.
  reset(); radio.rxResult = -6;
  assert(!recoverRadio()); expectPower();
  assert(radio.rxCalls + radio.standbyCalls == 1 && radioFailStreak == 5);
  assert(!Log.contains("radio re-init ok"));
  radio.rxResult = 0; clockMs += RADIO_RECOVER_MIN_MS;
  assert(recoverRadio()); expectPower();
  assert(radio.powers.size() == 2 && radioFailStreak == 0);
  std::cout << "radio power: " << EXPECTED_DBM
            << " dBm boot/recovery and failure paths passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="shore-radio-power-") as folder:
    source = Path(folder) / "test.cpp"
    source.write_text(cpp)
    for role, expected_dbm in [("CLIENT", 20), ("STATION", 17)]:
        executable = Path(folder) / role.lower()
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-DROLE_{role}", f"-DEXPECTED_DBM={expected_dbm}",
            str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
setup = block("void setup(")
assert setup.count("pmuOnline = initPmu()") == 1
assert setup.index("pmuOnline = initPmu()") < setup.index("if (!initRadio())")
print("radio power: ALDO3 boot ordering, 3300 mV readback, failure gating and recovery passed; legacy ATPC absent")
