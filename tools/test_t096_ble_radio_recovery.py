#!/usr/bin/env python3
"""Exercise actual BLE command dispatch after Ready radio initialization failure."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function
root = Path(__file__).resolve().parents[1]
source = (root/'src/t096_client.cpp').read_text()
body = extract_function(source, 'void serviceBle(uint32_t now)')
cpp = r'''
#include <cassert>
#include "t096_maintenance.h"
t096_maintenance::Fault maintenanceFault;
#include "t096_control.h"
bool bleReady=true, batteryProtection=false, radioReady=false, confirmationPending=false;
bool gnssEnabled=false, bleConnectionChanged=false;
uint32_t bleQueueDrops=0,nextBleStatusMs=0,nextBatteryMs=0;
uint16_t bleReplySeq=0; int16_t bleError=0,lastRadioInitError=-23;
size_t outLength=0; ble_control::Result bleResult=ble_control::Result::Idle;
enum class AirState {Idle,Sending,Receiving}; AirState air=AirState::Idle;
t096_control::Controller control(77);
ble_control::Command queued{}; bool pending=false; int bleQueue=1;
constexpr int pdTRUE=1;
int xQueueReceive(int,ble_control::Command *dst,int){if(!pending)return 0;*dst=queued;pending=false;return pdTRUE;}
unsigned calls=0; bool succeed=true;
bool configureRadio(){++calls;radioReady=succeed;return succeed;}
void setGnssEnabled(bool enabled){gnssEnabled=enabled;}
void publishBleStatus(uint32_t){}
template<class... T> void usbLog(const char*,T...){}
''' + body + r'''
int main(){
 control.setClientId(42);
 queued={ble_control::Op::Claim,0,42,9,77,1,100};pending=true;serviceBle(1);
 assert(calls==0 && control.state()==client_control::State::Ready);
 queued={ble_control::Op::Start,1,42,9,77,2,100};pending=true;serviceBle(2);
 assert(calls==1 && radioReady && gnssEnabled && confirmationPending && control.channel()==1);
 control.requestReady(); radioReady=false;gnssEnabled=false;confirmationPending=false;succeed=false;
 queued={ble_control::Op::Test,0,42,9,77,3,100};pending=true;serviceBle(3);
 assert(calls==2 && !radioReady && control.state()==client_control::State::Ready && control.command()==2);
 assert(bleResult==ble_control::Result::Rejected && bleError==-23 && !confirmationPending);
 succeed=true;pending=true;serviceBle(4);
 assert(calls==3 && radioReady && control.state()==client_control::State::Test && !gnssEnabled);
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td)/'test.cpp'; p.write_text(cpp); exe=Path(td)/'test'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS actual BLE Start/Test recovers failed radio; failed reinit rolls back and exposes error')
