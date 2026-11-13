#pragma once
// Included by the shared ESP32 station after its hardware/global declarations.
#include <time.h>
#include "phone_position.h"
#include "station_client_link.h"
#include "radio_profile.h"
#include "station_ble.h"
#include "runtime_radio_profile.h"

static bool gpsFixFresh();
static double stationLatitude();
static double stationLongitude();

namespace station_extensions {
static phone_position::Fix phone;
static station_client_link::Link link;
static bool txActive=false, rendezvous=false, commandViaBle=false;
static uint32_t txStarted=0, rendezvousMs=0, nextSendMs=0;
static uint32_t lastRfMs=0;
static bool haveRf=false;
static uint32_t lastLoraStateMs=0; static bool haveLoraState=false; static client_control::Status loraStatus{};
static uint8_t txBuffer[CLIENT_CONTROL_PACKET_LEN];
static uint32_t generation=1, operationId=0;
static station_ble::Event ble{};
static uint32_t bleMs=0, bleUptime=0, bleBoot=0; static bool haveBleClock=false;
static uint8_t selectedChannel=0, savedChannel=0, desiredChannel=0;
static int8_t activeChannel=-1;
static bool verified=false;
static const char *scanState="idle", *scanError="";
static uint32_t scanId=0, scanStarted=0;
static uint8_t scanChannel=0, scanAttempt=0;
static uint16_t scanSamples[2]{}, scanBusy[2]{}, scanErrors[2]{};
static bool scanning=false, cadActive=false;
static void logRf(const char *event, float frequency) {
  Log.print(F("[RF] "));Log.print(event);Log.print(F(" frequency="));Log.print(frequency,1);Log.println(F(" sf=9"));
}
static bool bleFresh(uint32_t now) { return ble.connected && ble.haveStatus && haveBleClock && uint32_t(now-bleMs)<3000 && ble.status.client==gpsClientId &&
    (!link.have || link.status.boot!=ble.status.boot || link.status.station!=nodeId || ble.status.owner!=nodeId || int16_t(ble.status.command-link.status.command)>=0); }
static bool claimed(uint32_t now) { return bleFresh(now) && ble.status.owner==nodeId && ble.status.stationBoot==controlBootId; }
static bool activeLora() { return loraStatus.state==client_control::State::Tracking || loraStatus.state==client_control::State::Test; }
static bool canRadio(uint32_t now) { return activeChannel>=0 && stationRxReady && claimed(now) && ble.status.state==client_control::State::Ready && !link.pending() && !scanning && !txActive; }
static void restoreRadio() {
  radio.standby(); stationRadioIrq=false; cadActive=false;
  if(radio.setFrequency(ble_control::frequencyMhz(desiredChannel))!=RADIOLIB_ERR_NONE) { activeChannel=-1;verified=false; }
  else activeChannel=desiredChannel;
  float frequency=activeChannel>=0?ble_control::frequencyMhz(activeChannel):0;
  runtime_radio_profile::set(frequency,9);logRf("scan restore",frequency);
  stationRxReady=activeChannel>=0 && radio.startReceive()==RADIOLIB_ERR_NONE;
  if(!stationRxReady) { scanState="error";scanError="radio_restore_failed";verified=false; }
  nextStationRxRetryMs=millis()+100;
}
static void abortScan(const char *why) { if(scanning) { scanning=false;scanState="error";scanError=why;restoreRadio(); } }
static void begin() {
  const uint8_t fallback=radio_profile::frequencyMhz<923.5f?0:1;
  savedChannel=prefs.getUChar("rfchannel",fallback); if(savedChannel>1)savedChannel=fallback;
  selectedChannel=savedChannel;desiredChannel=savedChannel;activeChannel=savedChannel; verified=false;
  if(radio.setFrequency(ble_control::frequencyMhz(savedChannel))!=RADIOLIB_ERR_NONE)activeChannel=-1;
  const float frequency=activeChannel>=0?ble_control::frequencyMhz(activeChannel):0;
  runtime_radio_profile::set(frequency,9);logRf("boot",frequency);
}
static void startBle() {
  if(!station_ble::begin(nodeId,controlBootId,gpsClientId,generation)) { ble.state=station_ble::State::Error;ble.error="worker_start_failed"; }
}
static void serviceBle() {
  station_ble::Event e{};
  if(!station_ble::poll(e) || e.generation!=generation)return;
  if(!e.haveStatus && ble.haveStatus) {
    e.haveStatus=true;e.status=ble.status;
    if(ble.state==station_ble::State::OwnedElsewhere) { e.state=ble.state;e.error="owned_elsewhere"; }
    ble=e; return;
  }
  ble=e;
  if(!e.haveStatus)return;
  if(!haveBleClock || bleBoot!=e.status.boot || bleUptime!=e.status.uptime) {
    bleBoot=e.status.boot;bleUptime=e.status.uptime;bleMs=millis();haveBleClock=true;
  }
  if(link.have && link.status.boot!=e.status.boot) { haveLoraState=false;verified=false; }
  if(haveLoraState && e.status.boot==loraStatus.boot && e.status.owner==nodeId && loraStatus.station==nodeId && int16_t(e.status.command-loraStatus.command)>0)haveLoraState=false;
  if(e.status.owner!=nodeId || e.status.stationBoot!=controlBootId)return;
  client_control::Status status{e.status.boot,e.status.state,e.status.owner,e.status.command,e.status.uptime,e.status.batteryMv,e.status.txPackets,e.status.rxErrors,(e.status.flags&1)!=0};
  // Preserve pending Start until an independent LoRa STATE proves the frequency.
  const bool pendingStart=link.pending() && link.action==client_control::Action::Start;
  link.observe(e.status.client,status,bleMs,nodeId);
  if(pendingStart && e.status.boot==link.requestedBoot && e.status.command==link.commandId)link.command="pending";
  if(link.pending() && e.status.boot==link.requestedBoot && int16_t(e.status.command-link.commandId)>0) { link.command="error";link.lastError=-2; }
  if(link.pending() && e.status.replySeq==link.commandId && e.status.stationBoot==controlBootId && e.status.result==ble_control::Result::Rejected) { link.command="error";link.lastError=e.status.error; }
}
static void serviceScan() {
  if(!scanning)return;
  if(!canRadio(millis()) && (!claimed(millis()) || ble.status.state!=client_control::State::Ready || link.pending())) { abortScan("client_unavailable");return; }
  if(cadActive) {
    if(!stationRadioIrq && uint32_t(millis()-scanStarted)<250)return;
    bool irq=stationRadioIrq;stationRadioIrq=false;cadActive=false;
    int16_t result=irq?radio.getChannelScanResult():RADIOLIB_ERR_RX_TIMEOUT;
    if(result==RADIOLIB_CHANNEL_FREE || result==RADIOLIB_LORA_DETECTED) { ++scanSamples[scanChannel];if(result==RADIOLIB_LORA_DETECTED)++scanBusy[scanChannel]; }
    else ++scanErrors[scanChannel];
    if(++scanAttempt>=5) { scanAttempt=0;if(++scanChannel>=2) { scanning=false;scanState=(scanSamples[0] && scanSamples[1])?"complete":"error";scanError=(scanSamples[0] && scanSamples[1])?"":"cad_no_samples";restoreRadio();return; } }
  }
  int16_t rc=radio.standby();
  if(rc==RADIOLIB_ERR_NONE)rc=radio.setFrequency(ble_control::frequencyMhz(scanChannel));
  stationRadioIrq=false;
  runtime_radio_profile::set(rc==RADIOLIB_ERR_NONE?ble_control::frequencyMhz(scanChannel):0,9);
  if(rc==RADIOLIB_ERR_NONE)rc=radio.startChannelScan();
  if(rc!=RADIOLIB_ERR_NONE) { ++scanErrors[scanChannel];scanning=false;scanState="error";scanError="cad_start_failed";restoreRadio();return; }
  stationRxReady=false;cadActive=true;scanStarted=millis();
}
static const char *helperUrl="https://n8886919.github.io/Shore-Spotter/phone-location.html";

static void resetClient() {
  abortScan("binding_changed"); link=station_client_link::Link{};rendezvous=false;haveRf=false;haveLoraState=false;verified=false;selectedChannel=savedChannel;scanState="idle";scanError="";memset(scanSamples,0,sizeof(scanSamples));memset(scanBusy,0,sizeof(scanBusy));memset(scanErrors,0,sizeof(scanErrors));haveBleClock=false; ble=station_ble::Event{}; ++generation;station_ble::setTarget(nodeId,controlBootId,gpsClientId,generation);
}
static bool accept(const uint8_t *b,size_t n,uint32_t now) {
  PacketHeader h{};
  if(!protocol::decodeHeader(b,n,h)||!isClientAllowed(h.clientId))return false;
  if(h.msgType==MSG_CLIENT_STATE) {
    client_control::Status s{};
    if(!client_control::decodeStatus(b,n,h,s))return false;
    const bool pendingStart=link.pending() && link.action==client_control::Action::Start;
    if(link.have && link.status.boot!=s.boot)verified=false;
    link.observe(h.clientId,s,now,nodeId);
    if(pendingStart && !strcmp(link.command,"confirmed") && (activeChannel<0 || activeChannel!=selectedChannel)) { link.command="error";link.lastError=-1; }
    if(link.action==client_control::Action::Start && !strcmp(link.command,"confirmed") && activeChannel==selectedChannel && s.boot==link.requestedBoot && s.station==nodeId && s.command==link.commandId) {
      verified=true;
      if(savedChannel!=selectedChannel) {
        if(prefs.putUChar("rfchannel",selectedChannel)==sizeof(uint8_t))savedChannel=selectedChannel;
        else { link.command="error";link.lastError=-1; }
      }
    }
    haveRf=true;lastRfMs=now;
    if(!(link.have && link.status.boot==s.boot && link.status.station==nodeId && s.station==nodeId && int16_t(s.command-link.status.command)<0)) {
      haveLoraState=true;lastLoraStateMs=now;loraStatus=s;
    }
    // STATE opens a short preamble listen, extended for an actual command.
    // Never transmit arbitrary repeated wake packets while the client is asleep.
    rendezvous=link.pending() && !commandViaBle && s.boot==link.requestedBoot;
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
  const uint32_t now=millis();link.tick(now);serviceBle();serviceScan();if(scanning)return;
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
  if(uint32_t(now-rendezvousMs)>client_control::kCommandStartLatestMs)return;
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
  const uint32_t now=millis();
  const bool loraFresh=haveLoraState && uint32_t(now-lastLoraStateMs)<90000 && !(ble.haveStatus && ble.status.owner && ble.status.owner!=nodeId);
  client_control::Status display=loraStatus;
  if(bleFresh(now)) display={ble.status.boot,ble.status.state,ble.status.owner,ble.status.command,ble.status.uptime,ble.status.batteryMv,ble.status.txPackets,ble.status.rxErrors,(ble.status.flags&1)!=0};
  const bool haveDisplay=bleFresh(now) || loraFresh;
  const uint32_t displayMs=bleFresh(now)?bleMs:lastLoraStateMs;
  const bool canControl=!link.pending() && !scanning && !txActive && (claimed(now) || (loraFresh && activeLora() && loraStatus.station==nodeId && loraStatus.boot==link.status.boot && loraStatus.command==link.status.command));
  const char *bs=ble.state==station_ble::State::Searching?"searching":ble.state==station_ble::State::Connecting?"connecting":ble.state==station_ble::State::Connected?"connected":ble.state==station_ble::State::OwnedElsewhere?"owned_elsewhere":ble.state==station_ble::State::Error?"error":"idle";
  String js=F("{\"api_version\":2,\"state_source\":\"");js+=bleFresh(now)?"ble":loraFresh?"lora":"none";
  js+=F("\",\"can_control\":");js+=canControl?"true":"false";
  js+=F(",\"station_boot_id\":");js+=String(controlBootId);js+=F(",\"operation_id\":");js+=String(operationId);
  js+=F(",\"ble\":{\"state\":\"");js+=bs;js+=F("\",\"connected\":");js+=ble.connected?"true":"false";
  js+=F(",\"claimed\":");js+=claimed(now)?"true":"false";js+=F(",\"owner_station_id\":");js+=ble.haveStatus?String(ble.status.owner):"0";
  js+=F(",\"last_error\":\"");js+=ble.error?ble.error:"";js+=F("\"},\"supported\":");js+=haveDisplay?"true":"false";
  js+=F(",\"client_id\":");js+=String(gpsClientId);
  js+=F(",\"boot_id\":");js+=haveDisplay?String(display.boot):"null";
  js+=F(",\"state\":\"");js+=haveDisplay?client_control::name(display.state):"unknown";
  js+=F("\",\"age_ms\":");js+=haveDisplay?String(uint32_t(now-displayMs)):"null";
  js+=F(",\"battery_mv\":");js+=haveDisplay&&display.batteryMv?String(display.batteryMv):"null";
  js+=F(",\"charging\":");js+=haveDisplay?(display.charging?"true":"false"):"null";
  js+=F(",\"command\":\"");js+=link.command;js+='"';
  js+=F(",\"command_id\":");js+=String(link.commandId);
  js+=F(",\"tx_packets\":");js+=String(display.txPackets);
  js+=F(",\"test_received\":");js+=String(link.probes);
  js+=F(",\"test_missing\":");js+=String(link.missing);
  js+=F(",\"test_rssi\":");js+=link.probes?String(link.rssi,1):"null";
  js+=F(",\"test_snr\":");js+=link.probes?String(link.snr,1):"null";
  js+=F(",\"test_max_gap_ms\":");js+=String(link.maxGapMs);
  js+=F(",\"last_error\":");js+=String(link.lastError);
  js+=F(",\"rf_group\":");js+=String(activeChannel>=0?activeChannel:savedChannel);
  js+=F(",\"frequency_mhz\":");js+=activeChannel>=0?String(ble_control::frequencyMhz(activeChannel),1):"null";
  js+=F(",\"storage_after_hours\":0,\"ready_poll_ms\":0,\"ble_advertising_ms\":10000}");return js;
}
static String radioJson() {
  String js=F("{\"selected\":");js+=String(selectedChannel);js+=F(",\"active\":");js+=activeChannel<0?"null":String(activeChannel);
  js+=F(",\"saved\":");js+=String(savedChannel);js+=F(",\"verified\":");js+=verified?"true":"false";
  js+=F(",\"scan_state\":\"");js+=scanState;js+=F("\",\"scan_id\":");js+=String(scanId);
  js+=F(",\"scan_error\":\"");js+=scanError;js+=F("\",\"can_scan\":");js+=canRadio(millis())?"true":"false";
  js+=F(",\"can_select\":");js+=canRadio(millis())?"true":"false";js+=F(",\"candidates\":[");
  for(uint8_t i=0;i<2;++i) { if(i)js+=',';js+=F("{\"id\":");js+=String(i);js+=F(",\"label\":\"");js+=ble_control::channelName(i);
    js+=F("\",\"frequency_mhz\":");js+=String(ble_control::frequencyMhz(i),1);js+=F(",\"sf\":9,\"bw_khz\":125,\"samples\":");js+=String(scanSamples[i]);
    js+=F(",\"busy\":");js+=String(scanBusy[i]);js+=F(",\"error\":");js+=String(scanErrors[i]);js+=F(",\"rssi_dbm\":null}"); }
  js+=F("]}");return js;
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
  httpServer.on("/api/client/radio",HTTP_GET,[](){ httpServer.sendHeader("Cache-Control","no-store");httpServer.send(200,"application/json",radioJson()); });
  httpServer.on("/api/client/radio",HTTP_POST,[](){
    if(!canRadio(millis())) { httpServer.send(409,"application/json","{\"error\":\"client_not_ready\"}");return; }
    String action=httpServer.arg("action");
    if(action=="select") { String c=httpServer.arg("channel");if(c!="0" && c!="1") {httpServer.send(400,"application/json","{\"error\":\"invalid_channel\"}");return;}selectedChannel=c=="1"?1:0;httpServer.send(200,"application/json",radioJson());return; }
    if(action=="scan") { memset(scanSamples,0,sizeof(scanSamples));memset(scanBusy,0,sizeof(scanBusy));memset(scanErrors,0,sizeof(scanErrors));scanChannel=scanAttempt=0;scanError="";scanState="scanning";scanning=true;cadActive=false;++scanId;logRf("scan begin",ble_control::frequencyMhz(activeChannel));httpServer.send(202,"application/json",radioJson());return; }
    httpServer.send(400,"application/json","{\"error\":\"invalid_action\"}");
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
    // Legacy API clients now stop into retained Ready, never SystemOFF.
    else if(a=="store")action=client_control::Action::Stop;
    else {httpServer.send(400,"application/json","{\"error\":\"unknown client action\"}");return;}
    const uint32_t now=millis();
    bool useBle=claimed(now);
    bool loraFresh=haveLoraState && uint32_t(now-lastLoraStateMs)<90000 && activeLora() && loraStatus.station==nodeId && loraStatus.boot==link.status.boot && loraStatus.command==link.status.command && !(ble.haveStatus && ble.status.owner && ble.status.owner!=nodeId);
    if(scanning || txActive || (!useBle && !loraFresh) ||
       ((action==client_control::Action::Start || action==client_control::Action::Test) &&
        ((!useBle && selectedChannel!=activeChannel) ||
         (useBle && ble.status.state!=client_control::State::Ready && ble.status.channel!=selectedChannel)))) {
      httpServer.send(409,"application/json","{\"error\":\"transport_unavailable\"}");return;
    }
    if(!link.request(action,now,nodeId)) {
      httpServer.send(409,"application/json","{\"error\":\"fresh paired client state required; another command or station may own the client\"}");return;
    }
    ++operationId;commandViaBle=useBle;
    if(useBle) {
      if(action==client_control::Action::Start || action==client_control::Action::Test) {
        desiredChannel=selectedChannel;
        int16_t rc=radio.standby(); stationRadioIrq=false;stationRxReady=false;nextStationRxRetryMs=now+100;
        if(rc==RADIOLIB_ERR_NONE)rc=radio.setFrequency(ble_control::frequencyMhz(selectedChannel));
        if(rc!=RADIOLIB_ERR_NONE) { activeChannel=-1;runtime_radio_profile::set(0,9);logRf("apply failed",0);verified=false;link.command="error";link.lastError=rc;httpServer.send(503,"application/json",controlJson());return; }
        activeChannel=selectedChannel;runtime_radio_profile::set(ble_control::frequencyMhz(activeChannel),9);logRf(action==client_control::Action::Start?"start":"test",ble_control::frequencyMhz(activeChannel));verified=false;stationRxReady=radio.startReceive()==RADIOLIB_ERR_NONE;
        if(!stationRxReady) { link.command="error";httpServer.send(503,"application/json",controlJson());return; }
      }
      ble_control::Op op=action==client_control::Action::Start?ble_control::Op::Start:action==client_control::Action::Test?ble_control::Op::Test:ble_control::Op::Stop;
      ble_control::Command c{op,selectedChannel,gpsClientId,nodeId,link.requestedBoot,link.commandId,controlBootId};
      if(!station_ble::request(generation,c)) { link.command="error";httpServer.send(503,"application/json",controlJson());return; }
    }
    httpServer.send(202,"application/json",controlJson());
  });
}
} // namespace station_extensions
