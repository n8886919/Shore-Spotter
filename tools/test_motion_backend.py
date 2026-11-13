#!/usr/bin/env python3
"""Compile the actual Station HTTP/motion functions with bounded UART fakes.
No serial, network or actuator I/O; test the Youmi M0 implementation, not a clone.
"""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/main.cpp').read_text()
def block(marker):
 start=s.index(marker);at=s.index('{',start);depth=1;end=at+1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
cpp=r'''
#include <cassert>
#include <cmath>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstring>
#include "alerts.h"
#include "servo_controller.h"
#include "servo_motion.h"
#include "tracking_policy.h"
#include "control_cadence.h"
#include "command_freshness.h"
#include "geo_math.h"
#define F(x) x
struct String:std::string { using std::string::string;String(const std::string&s):std::string(s){}String(uint32_t n):std::string(std::to_string(n)){}
 String(double n,unsigned char digits){std::ostringstream o;o<<std::fixed<<std::setprecision(digits)<<n;assign(o.str());}
 float toFloat()const{return std::strtof(c_str(),nullptr);}};
uint32_t clockMs=0;uint32_t millis(){return clockMs;}uint32_t micros(){return clockMs*1000;}uint32_t esp_random(){static uint32_t n=100;return ++n;}
template<class T>T constrain(T v,T lo,T hi){return std::max(lo,std::min(hi,v));}
struct LogStub {template<class T>void print(T){}template<class T>void println(T){}template<class...T>void printf(const char*,T...){}}Log;
struct Timer {void observe(uint32_t){}}controlGap;int motionDuration;
struct MeasureDuration {explicit MeasureDuration(int){}};
struct Serial {bool fault=false;int capacity=128;std::vector<uint32_t>times;std::string last;
 int availableForWrite(){return capacity;}size_t write(const uint8_t*p,size_t n){last.assign((const char*)p,n);times.push_back(clockMs);return fault?n-1:n;}}servoSerial;
namespace station_board {constexpr int kServoTxPin=43;}
using TrackMode=tracking_policy::Mode;
TrackMode trackMode=TrackMode::Manual,lastTrackingMode=TrackMode::Gps;
tracking_policy::Source controlSource=tracking_policy::Source::Hold;tracking_policy::Selector sourceSelector;
servo_motion::Controller servoMotion;servo_controller::Output servoOutput;control_cadence::GpsCadence gpsCadence;
command_freshness::HttpGate commandGate;
uint32_t controlBootId=42,rejectedMotionCommands=0,rejectedGpsSequence=0,lastRxMs=0;
float servoAngleDeg=179.5,servoTargetDeg=179.5,mountOffsetDeg=269.5,declinationDeg=0;
bool servoOutputReady=true,gpsFinishingTarget=false,mountCalibrated=true,declinationReady=true,havePkt=true,stationFix=true;
struct Data {bool fix=true;double lat=24.001,lon=121;}lastData;
bool gpsFixFresh(){return stationFix;}
bool feed=true;double gpsTarget=220;
static bool gpsModeAvailable();static bool gpsTrackingUsable();
void updateTracking(){if(trackMode==TrackMode::Gps&&controlSource==tracking_policy::Source::Gps&&gpsTrackingUsable())servoMotion.target(gpsTarget);}
float normalize360(float n){return geo::normalize360(n);}
struct Prefs {std::map<std::string,uint32_t>values;bool fail=false;int writes=0;
 void begin(const char*,bool){}uint32_t getUInt(const char*k,uint32_t d){return values.count(k)?values[k]:d;}
 size_t putUInt(const char*k,uint32_t v){++writes;if(fail)return 0;values[k]=v;return 4;}}prefs;
void loadGpsClientBinding(){}
struct Http {std::map<std::string,String>params;String body;int code=0;
 bool hasArg(const char*k){return params.count(k);}String arg(const char*k){return params[k];}int args(){return params.size();}
 String argName(int n){auto it=params.begin();std::advance(it,n);return it->first;}void send(int n,const char*,const String&b){code=n;body=b;}}httpServer;
'''
for marker in ['static const char *trackModeStr(', 'static uint32_t clientSampleAgeMs()', 'static bool clientFixFresh()', 'static bool haveBearingFix()', 'static bool gpsModeAvailable() {','static bool gpsTrackingUsable() {','static bool setServoAngle(float deg) {','static void enterManual() {','static void serviceControl() {','static bool selectTrackingMode(TrackMode mode) {','static bool parseAngleArgument(', 'static bool acceptMotionRequest() {','static void appendCommandContext(', 'static String controlReply() {','static String motionSettingsJson() {','static bool saveServoSpeed(', 'static bool requestTrackingMode(', 'static bool compassCalibrationReady() {','static float lockMountOffset(', 'static void appendServoJson(', 'static void loadStationSettings() {']:
 cpp+=block(marker)+'\n'
# Compile the actual SD alert branch, including its title/severity, without I/O.
sd_branch=s[s.index('  // SD failures belong'):s.index('  // --- 下水端：連線')]
cpp+='namespace sd_log {const char *state="recording";const char *stateName(){return state;}}\n'
cpp+='int sdAlerts(){int count=0;auto add=[&](alerts::Level level,const char *id,const char *,const char *){assert(level==alerts::WARN);assert(std::string(id)=="station_sd");++count;};\n'+sd_branch+'return count;}\n'
for path,name in [('/api/servo','manualHttp'),('/api/servo/mode','modeHttp'),('/api/servo/center','centerHttp'),('/api/servo/settings','speedHttp'),('/api/track/start','startHttp'),('/api/track/resume','resumeHttp'),('/api/track/pause','pauseHttp'),('/api/track/calibrate','calibrateHttp')]:
 m=re.search(r'httpServer.on\("'+re.escape(path)+r'",\s*HTTP_POST,\s*\[\]\(\)\s*{',s).group();b=block(m);cpp+='void '+name+'()'+b[b.index('{'):]+'\n'
cpp+=r'''
void auth(){httpServer.params.clear();httpServer.params["epoch"]=std::to_string(commandGate.epoch());httpServer.params["seq"]=std::to_string(commandGate.sequence()+1);httpServer.params["stamp"]=std::to_string(clockMs);}
void reset(){clockMs=0;servoOutput={};servoSerial={};servoMotion.initializeUs(179.5,0);servoMotion.setSpeed(30);
 servoOutputReady=mountCalibrated=declinationReady=havePkt=stationFix=feed=true;lastData={};lastRxMs=0;gpsTarget=220;
 servoAngleDeg=servoTargetDeg=179.5;gpsCadence={};enterManual();serviceControl();}
void run(uint32_t duration){const auto end=clockMs+duration;while(clockMs<end){clockMs+=10;if(feed)lastRxMs=clockMs;serviceControl();}}
int main(){
 for(const char *state:{"recording","mounting","stopping","stopped","unsupported"}){sd_log::state=state;assert(sdAlerts()==0);}
 sd_log::state="error";assert(sdAlerts()==1);sd_log::state="recording";assert(sdAlerts()==0);
 std::cout<<"PASS SD error alert, recovery, intentional stop and unsupported board exclusion\n";
 reset();assert(servoSerial.last=="#00P1500!");assert(servoOutput.channel()==0);
 auth();httpServer.params["mode"]="uart";modeHttp();assert(httpServer.code==400&&trackMode==TrackMode::Manual);
 auth();httpServer.params["angle"]="359";manualHttp();assert(httpServer.code==200);run(7000);
 assert(servoMotion.position()==359&&servoOutput.pulse()==2500&&servoAngleDeg==359);
 for(size_t i=1;i<servoSerial.times.size();++i)assert(servoSerial.times[i]-servoSerial.times[i-1]>=20);
 auth();httpServer.params["angle"]="360";manualHttp();assert(httpServer.code==400);
 auto target=servoMotion.requested();auth();httpServer.params["seq"]="0";manualHttp();assert(httpServer.code==409&&servoMotion.requested()==target);
 std::cout<<"PASS actual HTTP bounds, removed UART mode, freshness gate, 0-359 motion and <=50Hz M0 frames\n";
 reset();servoSerial.capacity=8;auth();httpServer.params["angle"]="300";manualHttp();run(100);
 assert(servoAngleDeg==179.5&&servoMotion.position()>179.5&&servoOutput.deferred()>0);
 servoSerial.capacity=128;run(100);assert(servoAngleDeg>179.5);
 servoSerial.fault=true;run(100);assert(!servoOutputReady&&trackMode==TrackMode::Paused&&servoMotion.faulted());
 assert(servoMotion.position()==servoAngleDeg);
 std::cout<<"PASS busy UART coalescing, short-write fault and last-sent hold\n";
 reset();auth();httpServer.params["speed"]="35";speedHttp();assert(httpServer.code==200&&servoMotion.speed()==35);
 const auto count=prefs.writes;auth();httpServer.params["speed"]="35";speedHttp();assert(prefs.writes==count);
 prefs.fail=true;auth();httpServer.params["speed"]="40";speedHttp();assert(httpServer.code==503&&servoMotion.speed()==35);prefs.fail=false;
 loadStationSettings();assert(servoMotion.speed()==35&&mountOffsetDeg==269.5);
 auth();httpServer.params["speed"]="91";speedHttp();assert(httpServer.code==400);
 std::cout<<"PASS speed persistence, reboot reload, duplicate writes and failed-save retention\n";
 reset();stationFix=false;auth();httpServer.params["mode"]="gps";modeHttp();assert(httpServer.code==409&&trackMode==TrackMode::Manual);
 stationFix=true;assert(selectTrackingMode(TrackMode::Gps));gpsTarget=359;run(100);
 assert(controlSource==tracking_policy::Source::Gps&&servoMotion.requested()==359);
 feed=false;run(2200);assert(controlSource==tracking_policy::Source::Hold&&gpsFinishingTarget);
 run(5000);assert(servoMotion.position()==359&&servoOutput.pulse()==2500&&!gpsFinishingTarget);
 auth();centerHttp();assert(httpServer.code==200&&trackMode==TrackMode::Manual&&servoMotion.requested()==179.5);run(7000);
 assert(compassCalibrationReady());auth();httpServer.params["bearing"]="90";calibrateHttp();assert(httpServer.code==200&&mountCalibrated&&std::abs(mountOffsetDeg+90.5)<0.01);
 std::cout<<"PASS dual-position gate, stale GPS freezes target and sends final pulse, centre/calibration at 179.5\n";
 reset();auth();httpServer.params["angle"]="250";manualHttp();run(5000);
 assert(!compassCalibrationReady());const auto epoch=commandGate.epoch();
 auth();httpServer.params["bearing"]="90";httpServer.params["physical_center"]="1";calibrateHttp();
 assert(httpServer.code==200&&servoMotion.position()==179.5&&servoMotion.requested()==179.5);
 assert(commandGate.epoch()!=epoch&&std::abs(mountOffsetDeg+90.5)<0.01);
 run(30);assert(servoAngleDeg==179.5&&servoSerial.last=="#00P1500!");
 auth();httpServer.params["angle"]="200";manualHttp();
 auth();httpServer.params["bearing"]="90";httpServer.params["physical_center"]="1";calibrateHttp();assert(httpServer.code==409);
 run(2000);assert(selectTrackingMode(TrackMode::Gps));
 auth();httpServer.params["bearing"]="90";httpServer.params["physical_center"]="1";calibrateHttp();assert(httpServer.code==409);
 enterManual();servoMotion.faultUs(micros());
 auth();httpServer.params["bearing"]="90";httpServer.params["physical_center"]="1";calibrateHttp();assert(httpServer.code==409);
 reset();auth();httpServer.params["bearing"]="360";httpServer.params["physical_center"]="1";calibrateHttp();assert(httpServer.code==400);
 auth();httpServer.params["bearing"]="90";httpServer.params["physical_center"]="0";calibrateHttp();assert(httpServer.code==400);
 std::cout<<"PASS physical centre resync from non-home angle, old epoch invalidation, moving/GPS/fault rejection and invalid inputs\n";
 String js="{";appendServoJson(js);js+="}";std::cout<<"JSON "<<js<<"\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 result=subprocess.check_output([str(p/'test')],text=True)
 import json
 for line in result.splitlines():
  if line.startswith('JSON '):
   payload=json.loads(line[5:]);assert payload['servo']['controller']['channel']==0;assert payload['servo']['controller']['actual_angle'] is None;assert payload['servo']['profile']=='youmi-359-uart-v1';print('PASS real Servo JSON schema and no false physical feedback')
  else:print(line)
assert 'httpServer.on("/api/track/prediction"' not in s
assert 'uartServoMode' not in s
assert 'ledcWrite(' not in s
print('PASS production removes prediction routes, UART input and direct PWM')
