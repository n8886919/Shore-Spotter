#pragma once
// The worker owns the BLE stack. Main-loop state crosses only bounded queues.
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <esp_log.h>
#include "ble_control_protocol.h"

namespace station_ble {
enum class State : uint8_t { Idle, Searching, Connecting, Connected, OwnedElsewhere, Error };
struct Target { uint16_t client, station; uint32_t stationBoot, generation; };
struct Request { uint32_t generation; ble_control::Command command; };
struct Event { uint32_t generation; State state; bool connected, haveStatus; ble_control::Status status; const char *error; };
static QueueHandle_t targets=nullptr, requests=nullptr, events=nullptr, matches=nullptr;
static SemaphoreHandle_t writeGate=nullptr;
struct Match { char address[18]; uint8_t type; uint32_t seenMs; };
static portMUX_TYPE targetMux=portMUX_INITIALIZER_UNLOCKED;
static Target callbackTarget{};
// Routed through sdk_log's bounded queue, never directly to USB/SD from this task.
static void trace(const Target &t,const char *phase,uint32_t started,int detail=0) {
  const uint32_t now=millis();
  esp_log_write(ESP_LOG_INFO,"shore-ble","[BLE] t_ms=%lu gen=%lu client=%04X phase=%s elapsed_ms=%lu detail=%d\n",
    (unsigned long)now,(unsigned long)t.generation,t.client,phase,(unsigned long)(now-started),detail);
}
static void publish(const Target &t, State s, bool connected, const ble_control::Status *status=nullptr, const char *error="") {
  static uint32_t lastGeneration=0;
  static State lastState=State::Idle;
  if(t.generation!=lastGeneration || s!=lastState || *error) {
    trace(t,*error?error:s==State::Searching?"searching":s==State::Connecting?"connecting":s==State::Connected?"claimed":"state",millis(),int(s));
    lastGeneration=t.generation;lastState=s;
  }
  Event e{t.generation,s,connected,status!=nullptr,{},error}; if(status)e.status=*status;
  xQueueOverwrite(events,&e);
}
class Advertisements : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    Target t; portENTER_CRITICAL(&targetMux); t=callbackTarget; portEXIT_CRITICAL(&targetMux);
    if (!t.client || !d.haveManufacturerData() || !d.isAdvertisingService(BLEUUID(ble_control::kServiceUuid))) return;
    String m=d.getManufacturerData();
    if(m.length()!=7 || uint8_t(m[0])!=0xff || uint8_t(m[1])!=0xff || m[2]!='S' || m[3]!='S' || uint8_t(m[4])!=1 || (uint16_t(uint8_t(m[5])) | uint16_t(uint8_t(m[6]))<<8)!=t.client)return;
    Match match{}; String addr=d.getAddress().toString(); strlcpy(match.address,addr.c_str(),sizeof(match.address)); match.type=d.getAddressType(); match.seenMs=millis();
    xQueueOverwrite(matches,&match);
  }
};
static Advertisements callbacks;
enum class Discovery { Matched, NoMatch, TargetChanged, StartFailed, StopFailed, TimedOut };
static Discovery discover(BLEScan *scan,Match &match) {
  xQueueReset(matches);
  // Async start lets the worker consume the first match without waiting out 2 s.
  // Only the worker stops the scan; never mutate scan storage in its callback.
  if(!scan->start(2,nullptr,false))return Discovery::StartFailed;
  const uint32_t started=millis();
  Target newer{};
  for(;;) {
    if(xQueuePeek(targets,&newer,0)==pdTRUE) {
      return scan->stop()?Discovery::TargetChanged:Discovery::StopFailed;
    }
    if(xQueueReceive(matches,&match,pdMS_TO_TICKS(50))==pdTRUE) {
      return scan->stop()?Discovery::Matched:Discovery::StopFailed;
    }
    if(!scan->isScanning())return Discovery::NoMatch;
    if(uint32_t(millis()-started)>=2200) {
      return scan->stop()?Discovery::TimedOut:Discovery::StopFailed;
    }
  }
}
static bool readStatus(BLERemoteCharacteristic *ch, uint16_t client, ble_control::Status &s) {
  String data=ch->readValue();
  return ble_control::decodeStatus(reinterpret_cast<const uint8_t *>(data.c_str()),data.length(),s) && s.client==client;
}
static bool send(BLERemoteCharacteristic *ch,const ble_control::Command &c) {
  uint8_t data[ble_control::kCommandSize]; return ble_control::encodeCommand(data,sizeof(data),c) && ch->writeValue(data,sizeof(data),true);
}
static void run(void *) {
  BLEDevice::init("Shore Station"); BLEDevice::setMTU(64);
  // Service + Client ID are in ADV itself; no need to wait for a scan response.
  BLEScan *scan=BLEDevice::getScan(); scan->setActiveScan(false);scan->setAdvertisedDeviceCallbacks(&callbacks);
  BLEClient *client=BLEDevice::createClient(); Target t{};
  for (;;) {
    if(xQueueReceive(targets,&t,0)==pdTRUE) { if(client->isConnected())client->disconnect(); xQueueReset(requests); }
    portENTER_CRITICAL(&targetMux); callbackTarget=t; portEXIT_CRITICAL(&targetMux);
    if(!t.client) { publish(t,State::Idle,false);vTaskDelay(pdMS_TO_TICKS(300));continue; }
    publish(t,State::Searching,false);
    Match match{}; Target newer{};
    const uint32_t scanStarted=millis();
    const Discovery found=discover(scan,match);
    const char *scanError=found==Discovery::StartFailed?"scan_start_failed":found==Discovery::StopFailed?"scan_stop_failed":found==Discovery::TimedOut?"scan_timeout":nullptr;
    if(scanError) { trace(t,scanError,scanStarted);publish(t,State::Error,false,nullptr,scanError);vTaskDelay(pdMS_TO_TICKS(500));continue; }
    if(xQueuePeek(targets,&newer,0)==pdTRUE)continue;
    if(found!=Discovery::Matched)continue;
    trace(t,"match",scanStarted,int(uint32_t(millis()-match.seenMs)));
    publish(t,State::Connecting,false);
    const uint32_t connectStarted=millis();
    // Pinned NimBLE ignores the third argument: internally 30000 ms + 1000 ms
    // task wait, with ble_gap_conn_cancel() on timeout. Do not claim a 5 s bound.
    // Other stacks use this explicit budget, covering the 10 s ADV interval.
    const bool connected=client->connect(BLEAddress(match.address),match.type,30000);
    trace(t,connected?"connect_ok":"connect_failed",connectStarted);
    if(!connected) { publish(t,State::Error,false,nullptr,"connect_failed");vTaskDelay(pdMS_TO_TICKS(500));continue; }
    if(xQueuePeek(targets,&newer,0)==pdTRUE) { client->disconnect();continue; }
    const uint32_t mtuStarted=millis();
    // The framework requests the local MTU automatically on connection.
    for(unsigned attempt=0;attempt<20 && client->getMTU()<64 && client->isConnected();++attempt)vTaskDelay(pdMS_TO_TICKS(100));
    trace(t,"mtu",mtuStarted,client->getMTU());
    if(client->getMTU()<64) { publish(t,State::Error,true,nullptr,"mtu_too_small");client->disconnect();continue; }
    const uint32_t gattStarted=millis();
    trace(t,"gatt_begin",gattStarted);
    BLERemoteService *service=client->getService(BLEUUID(ble_control::kServiceUuid));
    BLERemoteCharacteristic *sc=service?service->getCharacteristic(BLEUUID(ble_control::kStatusUuid)):nullptr;
    BLERemoteCharacteristic *cc=service?service->getCharacteristic(BLEUUID(ble_control::kCommandUuid)):nullptr;
    ble_control::Status s{};
    const bool statusOk=sc && cc && readStatus(sc,t.client,s);
    trace(t,"gatt_status",gattStarted,statusOk?0:!service?1:!sc?2:!cc?3:4);
    if(!statusOk) { publish(t,State::Error,false,nullptr,"status_read_failed");client->disconnect();continue; }
    if(s.owner && s.owner!=t.station) { publish(t,State::OwnedElsewhere,true,&s,"owned_elsewhere");client->disconnect();vTaskDelay(pdMS_TO_TICKS(2000));continue; }
    // A reconnect or Station restart always re-establishes this Station boot.
    ble_control::Command claim{ble_control::Op::Claim,s.channel,t.client,t.station,s.boot,uint16_t(s.command+1),t.stationBoot};
    const uint32_t claimStarted=millis();
    trace(t,"claim_begin",claimStarted);
    bool claimed=false;
    if(xSemaphoreTake(writeGate,portMAX_DELAY)==pdTRUE) {
      if(xQueuePeek(targets,&newer,0)!=pdTRUE)claimed=send(cc,claim);
      xSemaphoreGive(writeGate);
    }
    for(unsigned attempt=0;claimed && attempt<10;++attempt) {
      vTaskDelay(pdMS_TO_TICKS(100));
      if(readStatus(sc,t.client,s) && s.owner==t.station && s.stationBoot==t.stationBoot && s.command==claim.seq && s.result==ble_control::Result::Accepted && s.replySeq==claim.seq)break;
    }
    if(!claimed || s.owner!=t.station || s.stationBoot!=t.stationBoot || s.command!=claim.seq || s.result!=ble_control::Result::Accepted || s.replySeq!=claim.seq) {
      trace(t,"claim_failed",claimStarted,int(s.result));
      publish(t,State::Error,true,&s,"claim_failed");client->disconnect();continue;
    }
    trace(t,"claim_ok",claimStarted);
    publish(t,State::Connected,true,&s);
    uint32_t lastRead=millis();
    while(client->isConnected() && xQueuePeek(targets,&newer,0)!=pdTRUE) {
      Request req{};
      if(xQueueReceive(requests,&req,pdMS_TO_TICKS(100))==pdTRUE && req.generation==t.generation) {
        bool wrote=false;
        if(xSemaphoreTake(writeGate,portMAX_DELAY)==pdTRUE) {
          if(xQueuePeek(targets,&newer,0)!=pdTRUE && req.command.client==t.client && req.command.station==t.station && req.command.boot==s.boot && req.command.stationBoot==t.stationBoot)wrote=send(cc,req.command);
          xSemaphoreGive(writeGate);
        }
        if(!wrote) {
          publish(t,State::Error,true,&s,"write_failed");
        }
        lastRead=0;
      }
      if(uint32_t(millis()-lastRead)>=1000 || lastRead==0) {
        if(!readStatus(sc,t.client,s)) { publish(t,State::Error,true,nullptr,"status_read_failed");break; }
        publish(t,State::Connected,true,&s); lastRead=millis();
      }
    }
    if(client->isConnected())client->disconnect();
    publish(t,State::Searching,false);
  }
}
static bool begin(uint16_t station,uint32_t stationBoot,uint16_t client,uint32_t generation) {
  writeGate=xSemaphoreCreateMutex();
  targets=xQueueCreate(1,sizeof(Target)); requests=xQueueCreate(2,sizeof(Request));events=xQueueCreate(1,sizeof(Event));matches=xQueueCreate(1,sizeof(Match));
  if(!writeGate || !targets || !requests || !events || !matches)return false;
  Target t{client,station,stationBoot,generation};xQueueOverwrite(targets,&t);
  return xTaskCreate(run,"station-ble",8192,nullptr,1,nullptr)==pdPASS;
}
static void setTarget(uint16_t station,uint32_t stationBoot,uint16_t client,uint32_t generation) {
  if(!targets)return;
  Target t{client,station,stationBoot,generation};xQueueOverwrite(targets,&t);
}
static bool tryBindingLock() {return !writeGate || xSemaphoreTake(writeGate,0)==pdTRUE;}
static void unlockBinding() {if(writeGate)xSemaphoreGive(writeGate);}
static bool request(uint32_t generation,const ble_control::Command &c) {
  Request r{generation,c};return requests && xQueueSend(requests,&r,0)==pdTRUE;
}
static bool poll(Event &e) {return events && xQueueReceive(events,&e,0)==pdTRUE;}
}
