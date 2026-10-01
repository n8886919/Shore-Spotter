#pragma once
// Included by the shared ESP32 station after its hardware/global declarations.
#include <time.h>
#include "phone_position.h"
#include "station_client_link.h"
#include "radio_profile.h"

static bool gpsFixFresh();
static double stationLatitude();
static double stationLongitude();

namespace station_extensions {
static phone_position::Fix phone;
static station_client_link::Link link;
static bool txActive=false, rendezvous=false;
static uint32_t txStarted=0, rendezvousMs=0, nextSendMs=0;
static uint32_t lastRfMs=0;
static bool haveRf=false;
static uint8_t txBuffer[CLIENT_CONTROL_PACKET_LEN];
static const char *helperUrl="https://n8886919.github.io/Shore-Spotter/phone-location.html";

static void resetClient() {
  link=station_client_link::Link{};rendezvous=false;haveRf=false;
}
static bool accept(const uint8_t *b,size_t n,uint32_t now) {
  PacketHeader h{};
  if(!protocol::decodeHeader(b,n,h)||!isClientAllowed(h.clientId))return false;
  if(h.msgType==MSG_CLIENT_STATE) {
    client_control::Status s{};
    if(!client_control::decodeStatus(b,n,h,s))return false;
    link.observe(h.clientId,s,now,nodeId);
    haveRf=true;lastRfMs=now;
    // A state packet opens one 800 ms RX window on this client. Never transmit
    // arbitrary repeated wake packets while the client is asleep.
    rendezvous=link.pending() && s.boot==link.requestedBoot;
    rendezvousMs=now;nextSendMs=now+20;
    return true;
  }
  if(h.msgType==MSG_LINK_TEST) {
    client_control::Probe p{};
    if(!client_control::decodeProbe(b,n,h,p))return false;
    link.probe(p,now,receivedPacketRssi,receivedPacketSnr);
    haveRf=true;lastRfMs=now;return true;
  }
  return false;
}
static void serviceRadio() {
  const uint32_t now=millis();link.tick(now);
  if(txActive) {
    if(!stationRadioIrq && uint32_t(now-txStarted)<1000)return;
    const bool irq=stationRadioIrq;
    stationRadioIrq=false;
    const uint32_t flags=radio.getIrqFlags();
    const int16_t finish=radio.finishTransmit();
    if(!irq || !(flags & RADIOLIB_SX126X_IRQ_TX_DONE) || finish!=RADIOLIB_ERR_NONE) {
      link.lastError=finish==RADIOLIB_ERR_NONE?RADIOLIB_ERR_TX_TIMEOUT:finish;
    }
    txActive=false;setStationRadioTransmit(false);
    stationRxReady=radio.startReceive()==RADIOLIB_ERR_NONE;
    nextStationRxRetryMs=now+100;
    return;
  }
  if(!rendezvous || !link.pending() || !loop_metrics::due(now,nextSendMs))return;
  rendezvous=false;
  // Do not start after a stalled HTTP request has consumed the receive window.
  if(uint32_t(now-rendezvousMs)>120)return;
  const PacketHeader h{link.client,link.commandId,MSG_CLIENT_CONTROL};
  const client_control::Command c{link.requestedBoot,link.action,nodeId};
  const size_t n=client_control::encodeCommand(txBuffer,sizeof(txBuffer),h,c);
  if(!n)return;
  radio.standby();setStationRadioTransmit(true);stationRadioIrq=false;
  const int16_t result=radio.startTransmit(txBuffer,n);
  if(result==RADIOLIB_ERR_NONE) {
    txActive=true;txStarted=now;stationRxReady=false;
  } else {
    link.lastError=result;setStationRadioTransmit(false);
    stationRxReady=radio.startReceive()==RADIOLIB_ERR_NONE;
    nextStationRxRetryMs=now+100;
  }
}
static String positionJson() {
  String js=F("{\"source\":\"");js+=phone.enabled?"phone":"gnss";
  js+=F("\",\"valid\":");js+=gpsFixFresh()?"true":"false";
  js+=F(",\"lat\":");js+=gpsFixFresh()?String(stationLatitude(),7):"null";
  js+=F(",\"lon\":");js+=gpsFixFresh()?String(stationLongitude(),7):"null";
  js+=F(",\"accuracy_m\":");js+=phone.enabled?String(phone.accuracy,1):"null";
  js+=F(",\"updated_utc_ms\":");js+=phone.enabled?String(phone.utcMs,0):"null";
  js+=F(",\"age_ms\":");js+=phone.enabled?String(phone.age(millis())):"null";
  js+=F(",\"helper_url\":\"");js+=helperUrl;js+=F("\",\"retention\":\"until_station_restart\"}");
  return js;
}
static String controlJson() {
  link.tick(millis());
  String js=F("{\"supported\":");js+=link.fresh(millis())?"true":"false";
  js+=F(",\"client_id\":");js+=String(gpsClientId);
  js+=F(",\"boot_id\":");js+=link.have?String(link.status.boot):"null";
  js+=F(",\"state\":\"");js+=link.have?client_control::name(link.status.state):"unknown";
  js+=F("\",\"age_ms\":");js+=link.have?String(uint32_t(millis()-link.receivedMs)):"null";
  js+=F(",\"battery_mv\":");js+=link.have&&link.status.batteryMv?String(link.status.batteryMv):"null";
  js+=F(",\"charging\":");js+=link.have?(link.status.charging?"true":"false"):"null";
  js+=F(",\"command\":\"");js+=link.command;js+='"';
  js+=F(",\"command_id\":");js+=String(link.commandId);
  js+=F(",\"tx_packets\":");js+=String(link.status.txPackets);
  js+=F(",\"test_received\":");js+=String(link.probes);
  js+=F(",\"test_missing\":");js+=String(link.missing);
  js+=F(",\"test_rssi\":");js+=link.probes?String(link.rssi,1):"null";
  js+=F(",\"test_snr\":");js+=link.probes?String(link.snr,1):"null";
  js+=F(",\"test_max_gap_ms\":");js+=String(link.maxGapMs);
  js+=F(",\"last_error\":");js+=String(link.lastError);
  js+=F(",\"rf_group\":");js+=String(radio_profile::group);
  js+=F(",\"frequency_mhz\":");js+=String(radio_profile::frequencyMhz,1);
  js+=F(",\"storage_after_hours\":12}");return js;
}
static bool numberArg(const char *key,double &v) {
  if(!httpServer.hasArg(key))return false;
  const String t=httpServer.arg(key);char *end=nullptr;
  v=strtod(t.c_str(),&end);return t.length() && end && *end=='\0' && isfinite(v);
}
static void registerRoutes() {
  httpServer.on("/api/station/position",HTTP_GET,[](){
    httpServer.sendHeader("Cache-Control","no-store");
    httpServer.send(200,"application/json",positionJson());
  });
  httpServer.on("/api/station/position",HTTP_POST,[](){
    const String a=httpServer.arg("action");
    if(a=="gnss")phone.enabled=false;
    else if(a=="phone") {
      double la,lo,ac,utc;
      if(!numberArg("lat",la)||!numberArg("lon",lo)||!numberArg("accuracy",ac)||
          !numberArg("timestamp",utc)||!phone.set(la,lo,ac,utc,millis())) {
        httpServer.send(400,"application/json","{\"error\":\"invalid or older phone position\"}");return;
      }
    } else {httpServer.send(400,"application/json","{\"error\":\"action must be phone or gnss\"}");return;}
    httpServer.send(200,"application/json",positionJson());
  });
  httpServer.on("/api/client/control",HTTP_GET,[](){
    httpServer.sendHeader("Cache-Control","no-store");
    httpServer.send(200,"application/json",controlJson());
  });
  httpServer.on("/api/client/control",HTTP_POST,[](){
    const String a=httpServer.arg("action");
    client_control::Action action;
    if(a=="start")action=client_control::Action::Start;
    else if(a=="stop")action=client_control::Action::Stop;
    else if(a=="test")action=client_control::Action::Test;
    else if(a=="store")action=client_control::Action::Store;
    else {httpServer.send(400,"application/json","{\"error\":\"unknown client action\"}");return;}
    if(action==client_control::Action::Store && link.have && link.status.charging) {
      httpServer.send(409,"application/json","{\"error\":\"disconnect USB power before deep sleep; stop remains available\"}");return;
    }
    if(!link.request(action,millis(),nodeId)) {
      httpServer.send(409,"application/json","{\"error\":\"fresh paired client state required; another command or station may own the client\"}");return;
    }
    httpServer.send(202,"application/json",controlJson());
  });
}
} // namespace station_extensions
