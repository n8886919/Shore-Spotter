# Host integration of actual HTTP handlers, GPS prediction, persistence and PWM.
# Run: python3 tools/test_motion_backend.py (no board or Arduino required).
from pathlib import Path
import re,subprocess,tempfile
p=Path(tempfile.mkdtemp(prefix='shore-backend-test-'));source=Path('src/main.cpp').read_text()
def block(marker):
 start=source.index(marker);body=source.index('{',start);depth=0
 for m in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',source[body:]):
  token=m.group()
  if token=='{':depth+=1
  if token=='}':
   depth-=1
   if not depth:return source[start:body+m.end()]
 raise ValueError(marker)
cpp=r'''
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <iostream>
#include "servo_motion.h"
#include "servo_profile.h"
#include "uart_target.h"
#include "command_freshness.h"
#include "tracking_policy.h"
#include "loop_metrics.h"
#include "control_cadence.h"
#include "gnss_snapshot.h"
#define F(x) x
#define ESP_ARDUINO_VERSION_MAJOR 3
constexpr int SERVO_PIN=21;
using std::isfinite;
template<class T>T constrain(T x,T a,T b){return std::min(std::max(x,a),b);}
struct String:std::string{using std::string::string;String(const std::string&s):std::string(s){}explicit String(uint32_t n):std::string(std::to_string(n)){}float toFloat()const{return std::strtof(c_str(),nullptr);}};
uint32_t clockMs=0,subMsUs=0,epochCounter=100,lastRxMs=0,controlBootId=42;
uint32_t dataAirtimeMs=0;
double radians(double degrees){return degrees*3.14159265358979323846/180.0;}
uint32_t micros(){return clockMs*1000u+subMsUs;}
uint32_t millis(){return clockMs;}uint32_t esp_random(){return ++epochCounter;}
struct LogStub{template<class T>void print(T){}template<class T>void println(T){}}Log;
struct Clock{static uint32_t micros(){return ::micros();}};
using MeasureDuration=loop_metrics::Measure<Clock>;
loop_metrics::Duration motionDuration;loop_metrics::Gap controlGap;
using TrackMode=tracking_policy::Mode;
TrackMode trackMode=TrackMode::Manual,lastTrackingMode=TrackMode::Uart;
tracking_policy::Source controlSource=tracking_policy::Source::Hold;
tracking_policy::Selector sourceSelector;servo_motion::Controller servoMotion;
control_cadence::GpsCadence gpsCadence;
command_freshness::HttpGate commandGate;
uint32_t rejectedMotionCommands=0,servoLastDuty=UINT32_MAX;
float servoAngleDeg=90,servoTargetDeg=90,gpsTarget=120,mountOffsetDeg=0;
bool servoPwmReady=true,writeFails=false,mountCalibrated=false,declinationReady=false;
bool serverFix=true,havePkt=true,gpsFeed=true;
bool gpsPredictionEnabled=true;
struct Value{bool valid=true;double data=8;uint32_t ageMs=0;
 bool isValid()const{return valid;}double value()const{return data;}double hdop()const{return data;}uint32_t age()const{return ageMs;}};
struct Gps{Value satellites,hdop{true,1.2,0},location;}gps;
struct Data{bool fix=true,velocityValid=true;uint8_t satellites=8,hdop10=12,age10ms=0;double lat=24,lon=121;uint16_t speedCmS=500,courseDeg10=900;}lastData;
struct CollectorStub{bool sample(uint32_t,gnss_snapshot::Snapshot&s){
 s.fix=serverFix;s.satellites=gps.satellites.valid?gps.satellites.data:255;
 s.hdop=gps.hdop.valid?gps.hdop.data:NAN;
 s.sourceAgeMs=std::max(gps.location.ageMs,std::max(gps.satellites.ageMs,gps.hdop.ageMs));return true;}}gnssCollector;
bool gpsFixFresh(){return serverFix;}
int writes=0;
bool ledcWrite(int,uint32_t){if(writeFails)return false;++writes;return true;}
static bool gpsModeAvailable();static bool gpsTrackingUsable();
void updateTracking(){if(trackMode==TrackMode::Gps&&controlSource==tracking_policy::Source::Gps&&gpsTrackingUsable())servoMotion.target(gpsTarget);}
const char*trackModeStr(TrackMode){return "mode";}
String controlReply(){return "{}";}String motionSettingsJson(){return "{}";}
void loadGpsClientBinding(){}
struct Endpoint{
 uart_target::Mailbox mailbox;
 void enter(uint32_t){leave();}void leave(){mailbox=uart_target::Mailbox{};}
 void poll(){mailbox.expire(millis());}bool ready(){return mailbox.ready();}
 int32_t targetMdeg(){return mailbox.target();}
}uartServoMode;
struct Http{
 std::map<std::string,String> params;int code=0;String body;
 bool hasArg(const char*k){return params.count(k);}String arg(const char*k){return params[k];}
 int args(){return params.size();}String argName(int i){auto it=params.begin();std::advance(it,i);return it->first;}
 void send(int c,const char*,const String&b){code=c;body=b;}
 void sendHeader(const char*,const char*){}
}httpServer;
struct Prefs{
 std::map<std::string,uint32_t> values;int writes=0;bool fail=false,dropWrite=false;
 void begin(const char*,bool){}
 size_t putUInt(const char*k,uint32_t value){++writes;if(fail)return 0;if(!dropWrite)values[k]=value;return sizeof(value);}
 uint32_t getUInt(const char*k,uint32_t fallback){return values.count(k)?values[k]:fallback;}
}prefs;
'''
for name in ['GPS_PREDICTION_KEY','DR_MIN_SPEED_CMS','DR_MAX_AGE_S','GPS_BACKLOG_GUARD_MS']:
 cpp+=re.search(r'constexpr[^;\n]*\b'+name+r'\b[^;]*;',source).group()+'\n'
for marker in ['static uint32_t clientSampleAgeMs()','static bool clientFixFresh()','static bool haveBearingFix()','static bool setServoAngle(float deg) {','static bool gpsModeAvailable() {','static bool gpsTrackingUsable() {','static void enterManual() {','static void serviceControl() {','static bool selectTrackingMode(TrackMode mode) {','static bool parseAngleArgument(','static bool acceptMotionRequest() {','static bool saveServoSpeed(double speed) {','static bool requestTrackingMode(TrackMode mode) {','static void loadServerSettings() {','static void predictClientPos(','static void appendCommandContext(','static String gpsPredictionJson() {','static bool saveGpsPrediction(']:
 cpp+=block(marker)+'\n'
for path,name in [('/api/servo','manualHttp'),('/api/servo/mode','modeHttp'),('/api/servo/settings','settingsHttp'),('/api/track/start','startHttp'),('/api/track/resume','resumeHttp'),('/api/track/prediction','predictionHttp')]:
 marker=re.search(r'httpServer.on\("'+re.escape(path)+r'",\s*HTTP_POST,\s*\[\]\(\)\s*{',source).group()
 b=block(marker);cpp+='void '+name+'()'+b[b.index('{'):]+'\n'
marker=re.search(r'httpServer.on\("/api/track/prediction",\s*HTTP_GET,\s*\[\]\(\)\s*{',source).group()
b=block(marker);cpp+='void predictionGet()'+b[b.index('{'):]+'\n'
cpp+=r'''
void auth(){httpServer.params.clear();httpServer.params["epoch"]=std::to_string(commandGate.epoch());
 httpServer.params["seq"]=std::to_string(commandGate.sequence()+1);httpServer.params["stamp"]=std::to_string(clockMs);}
void reset(){clockMs=subMsUs=0;writes=0;writeFails=false;servoPwmReady=true;serverFix=havePkt=gpsFeed=true;
 gps=Gps{};lastData=Data{};lastRxMs=0;dataAirtimeMs=0;mountCalibrated=declinationReady=false;gpsPredictionEnabled=true;
 gpsCadence={};servoLastDuty=servo_profile::dutyForAngle(90);servoMotion.initializeUs(90,0);
 servoMotion.setSpeed(30);servoAngleDeg=servoTargetDeg=90;enterManual();lastTrackingMode=TrackMode::Uart;}
void run(uint32_t duration){const auto end=clockMs+duration;while(clockMs<end){clockMs+=10;if(gpsFeed)lastRxMs=clockMs;serviceControl();}}
int main(){
 reset();assert(selectTrackingMode(TrackMode::Uart));assert(trackMode==TrackMode::Uart&&!uartServoMode.ready());
 run(1000);assert(writes==0&&servoAngleDeg==90);
 for(int i=0;i<100;i++){uartServoMode.mailbox.set(120000,clockMs);run(10);}
 assert(servoAngleDeg>119&&servoAngleDeg<=120);run(300);float held=servoAngleDeg;run(500);assert(servoAngleDeg==held);
 std::cout<<"PASS default UART selection, fresh input, shared rate and expiry hold\n";
 reset();auth();httpServer.params["angle"]="90.02";manualHttp();run(10);assert(servoAngleDeg>90.019);
 auth();httpServer.params["angle"]="180";manualHttp();run(100);float before=servoAngleDeg;
 auth();httpServer.params["angle"]="0";manualHttp();run(10);assert(std::abs(servoAngleDeg-(before-.3))<1e-4);
 auto goal=servoMotion.requested();auto mode=trackMode;
 auth();httpServer.params["speed"]="20";settingsHttp();assert(httpServer.code==200&&servoMotion.speed()==20);
 assert(prefs.writes==1&&prefs.getUInt(servo_motion::kSpeedKey,0)==20000&&servoMotion.requested()==goal&&trackMode==mode);
 auth();httpServer.params["speed"]="20";settingsHttp();assert(httpServer.code==200&&prefs.writes==1);
 reset();loadServerSettings();assert(servoMotion.speed()==20);selectTrackingMode(TrackMode::Uart);
 auth();httpServer.params["speed"]="29.5";settingsHttp();assert(httpServer.code==200&&servoMotion.speed()==29.5&&trackMode==TrackMode::Uart);
 prefs.fail=true;auth();httpServer.params["speed"]="90";settingsHttp();assert(httpServer.code==503&&servoMotion.speed()==29.5);
 prefs.fail=false;auth();httpServer.params["speed"]="91";settingsHttp();assert(httpServer.code==400);
 auth();httpServer.params["speed"]="30";httpServer.params["action"]="apply";settingsHttp();assert(httpServer.code==400);
 reset();loadServerSettings();assert(servoMotion.speed()==29.5);
 prefs.values.clear();prefs.values["motioncfg"]=90000;loadServerSettings();assert(servoMotion.speed()==30);
 std::cout<<"PASS live automatic speed persistence, reboot reload, no duplicate writes, failed-save retention and old profile retirement\n";
 reset();gps.satellites.data=6;gps.hdop.data=3;lastData.satellites=6;lastData.hdop10=30;
 assert(gpsModeAvailable());selectTrackingMode(TrackMode::Uart);lastData.fix=false;
 auto epoch=commandGate.epoch();auth();httpServer.params["mode"]="gps";modeHttp();
 assert(httpServer.code==409&&trackMode==TrackMode::Uart&&commandGate.epoch()==epoch);
 auth();startHttp();assert(httpServer.code==409&&trackMode==TrackMode::Uart);
 lastTrackingMode=TrackMode::Gps;auth();resumeHttp();assert(httpServer.code==409&&trackMode==TrackMode::Uart);
 lastData.fix=true;gps.hdop.ageMs=2000;assert(!gpsModeAvailable());gps.hdop.ageMs=0;
 gps.location.ageMs=2000;assert(!gpsModeAvailable());gps.location.ageMs=0;
 lastData.hdop10=255;assert(!gpsModeAvailable());lastData.hdop10=30;
 serverFix=false;assert(!gpsModeAvailable());serverFix=true;
 gps.satellites.data=5;assert(!gpsModeAvailable());gps.satellites.data=6;
 gps.hdop.data=NAN;assert(!gpsModeAvailable());gps.hdop.data=3;
 auth();httpServer.params["mode"]="gps";modeHttp();assert(httpServer.code==200&&trackMode==TrackMode::Gps);
 run(3000);assert(servoAngleDeg==90);mountCalibrated=declinationReady=true;
 run(2000);assert(servoAngleDeg==90);run(1500);assert(std::abs(servoAngleDeg-120)<1e-4);
 gpsFeed=false;run(2000);assert(controlSource==tracking_policy::Source::Hold&&trackMode==TrackMode::Gps);
 std::cout<<"PASS actual dual-GPS Good/OK gate, Bad/Miss/stale rejection on mode/start/resume, calibration and loss hold\n";
 reset();auth();httpServer.params["angle"]="150";manualHttp();serviceControl();
 subMsUs=100;serviceControl();assert(writes==0);subMsUs=400;serviceControl();assert(writes==1);
 int previous=writes;serviceControl();assert(writes==previous);
 writeFails=true;clockMs=10;serviceControl();assert(trackMode==TrackMode::Paused&&servoMotion.faulted());
 assert(servoMotion.position()==servoAngleDeg);
 std::cout<<"PASS uncapped microsecond PWM, duplicate suppression and write-failure hold\n";
 reset();selectTrackingMode(TrackMode::Uart);auto oldEpoch=commandGate.epoch();enterManual();
 auth();httpServer.params["epoch"]=std::to_string(oldEpoch);httpServer.params["angle"]="0";manualHttp();assert(httpServer.code==409);
 std::cout<<"PASS stale HTTP epoch rejection\n";
 reset();prefs.values.clear();prefs.writes=0;loadServerSettings();assert(gpsPredictionEnabled);
 predictionGet();assert(httpServer.code==200&&httpServer.body.find("\"enabled\":true,\"alpha\":1")!=std::string::npos);
 assert(httpServer.body.find("\"control_boot_id\":42")!=std::string::npos);
 for(auto m:{TrackMode::Manual,TrackMode::Uart,TrackMode::Gps,TrackMode::Paused}){
  if(m==TrackMode::Manual||m==TrackMode::Paused){enterManual();trackMode=m;}else assert(selectTrackingMode(m));
  auto savedEpoch=commandGate.epoch();auto savedTarget=servoMotion.requested();
  auto savedSource=controlSource;auto savedAngle=servoAngleDeg;auto savedSpeed=servoMotion.speed();auto savedWrites=writes;
  for(bool enabled:{false,true}){
   auth();httpServer.params["enabled"]=enabled?"1":"0";predictionHttp();
   assert(httpServer.code==200&&gpsPredictionEnabled==enabled&&trackMode==m&&controlSource==savedSource);
   assert(commandGate.epoch()==savedEpoch&&servoMotion.requested()==savedTarget&&servoAngleDeg==savedAngle);
   assert(servoMotion.speed()==savedSpeed&&writes==savedWrites);
   const auto body=std::string("\"enabled\":")+(enabled?"true,\"alpha\":1":"false,\"alpha\":0");
   assert(httpServer.body.find(body)!=std::string::npos);
  }
 }
 auto persistedWrites=prefs.writes;auth();httpServer.params["enabled"]="1";predictionHttp();
 assert(httpServer.code==200&&prefs.writes==persistedWrites);
 auth();httpServer.params["enabled"]="0";predictionHttp();assert(httpServer.code==200&&!gpsPredictionEnabled);
 reset();loadServerSettings();assert(!gpsPredictionEnabled);
 prefs.values[GPS_PREDICTION_KEY]=9;loadServerSettings();assert(gpsPredictionEnabled);
 prefs.values.erase(GPS_PREDICTION_KEY);gpsPredictionEnabled=false;loadServerSettings();assert(gpsPredictionEnabled);
 std::cout<<"PASS actual prediction GET/POST, all modes isolated, no immediate PWM, reboot/default and duplicate-save behavior\n";
 for(const char*invalid:{"", "true", "2", "0.0", " 1", "1x", "-1"}){
  auth();httpServer.params["enabled"]=invalid;predictionHttp();assert(httpServer.code==400&&gpsPredictionEnabled);
 }
 auth();predictionHttp();assert(httpServer.code==400);
 auth();httpServer.params["enabled"]="0";httpServer.params["alpha"]="0.5";predictionHttp();assert(httpServer.code==400);
 auto rejectedWrites=prefs.writes;httpServer.params.clear();httpServer.params["enabled"]="0";
 predictionHttp();assert(httpServer.code==409);
 clockMs=3000;auth();httpServer.params["enabled"]="0";httpServer.params["stamp"]="1000";
 predictionHttp();assert(httpServer.code==409);
 auth();httpServer.params["enabled"]="0";httpServer.params["seq"]=std::to_string(commandGate.sequence());
 predictionHttp();assert(httpServer.code==409);
 auth();httpServer.params["enabled"]="0";httpServer.params["epoch"]=std::to_string(commandGate.epoch()-1);
 predictionHttp();assert(httpServer.code==409&&prefs.writes==rejectedWrites&&gpsPredictionEnabled);
 prefs.fail=true;auth();httpServer.params["enabled"]="0";predictionHttp();assert(httpServer.code==503&&gpsPredictionEnabled);
 prefs.fail=false;prefs.dropWrite=true;auth();httpServer.params["enabled"]="0";predictionHttp();
 assert(httpServer.code==503&&gpsPredictionEnabled);prefs.dropWrite=false;
 std::cout<<"PASS prediction strict arguments, stale/replay rejection and persistence write/readback failure retention\n";
 reset();clockMs=1500;lastRxMs=1000;double lat,lon;predictClientPos(lat,lon);
 assert(std::abs(lat-24)<1e-10&&std::abs((lon-121)*111320*std::cos(radians(24))-2.5)<1e-6);
 gpsPredictionEnabled=false;predictClientPos(lat,lon);assert(lat==24&&lon==121);
 gpsPredictionEnabled=true;clockMs=8000;lastData.courseDeg10=0;predictClientPos(lat,lon);
 assert(lat==24&&lon==121); // expired position cannot be projected
 clockMs=1500;lastData.velocityValid=false;predictClientPos(lat,lon);assert(lat==24&&lon==121);
 lastData.velocityValid=true;lastData.age10ms=50;dataAirtimeMs=165;predictClientPos(lat,lon);
 assert(clientSampleAgeMs()==1165&&std::abs((lat-24)*111320-5.825)<1e-6);
 lastData.age10ms=150;predictClientPos(lat,lon);assert(lat==24&&lon==121&&!gpsModeAvailable());
 lastData.age10ms=255;assert(!clientFixFresh());lastData.age10ms=0;dataAirtimeMs=0;
 lastData.speedCmS=29;predictClientPos(lat,lon);assert(lat==24&&lon==121);
 lastData.speedCmS=500;lastRxMs=UINT32_MAX-249;clockMs=250;predictClientPos(lat,lon);
 assert(std::abs((lat-24)*111320-2.5)<1e-7);
 std::cout<<"PASS actual predictor alpha=0/1, source+RF+receive age, expiry, velocity validity, low-speed bypass and millis wrap\n";
}
'''
assert '  selectTrackingMode(TrackMode::Uart);  // default source; wait for fresh UART input' in source
assert '/api/servo/diagnostics' not in source and 'WEB_LOG_HTML' not in source
(p/'runtime_integration.cpp').write_text(cpp)
cmd=['c++','-std=c++17','-Wall','-Wextra','-O2','-I',str(Path('include').resolve()),str(p/'runtime_integration.cpp'),'-o',str(p/'runtime_integration')]
r=subprocess.run(cmd,capture_output=True,text=True);assert r.returncode==0,r.stderr
r=subprocess.run([str(p/'runtime_integration')],capture_output=True,text=True)
(p/'runtime_integration.log').write_text(r.stdout+r.stderr);print(r.stdout,end='');assert r.returncode==0,r.stderr
