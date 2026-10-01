#!/usr/bin/env python3
"""Real Station idle-client guard with SDK-shaped sockets and clock; no network."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='shore-http-idle-') as td:
 d=Path(td)
 (d/'WebServer.h').write_text(r'''
#pragma once
#include <functional>
#include <string>
#include <cstdint>
enum HTTPClientStatus {HC_NONE,HC_WAIT_READ,HC_WAIT_CLOSE};
struct Client {int bytes=0;bool stopped=false;int available(){return bytes;}void stop(){stopped=true;}};
struct Listener {bool queued=false;bool hasClient(){return queued;}};
class WebServer {
 public:
  using Next=std::function<bool()>;using Middleware=std::function<bool(WebServer&,Next)>;
  explicit WebServer(int){}virtual ~WebServer()=default;
  void addMiddleware(Middleware cb){middleware=cb;}
  std::string uri(){return "/api/status";}
  virtual void handleClient(){++polls;if(_currentClient.bytes){middleware(*this,[](){return true;});_currentClient.bytes=0;_currentStatus=HC_NONE;}}
  void pending(uint32_t at,int bytes=0,HTTPClientStatus state=HC_WAIT_READ){_statusChange=at;_currentClient={bytes,false};_currentStatus=state;}
  bool stopped(){return _currentClient.stopped;}unsigned polls=0;
  void queued(bool value){_server.queued=value;}
 protected:
  virtual size_t _currentClientWrite(const char*,size_t n){return n;}
  virtual size_t _currentClientWrite_P(const char*,size_t n){return n;}
  Client _currentClient;HTTPClientStatus _currentStatus=HC_NONE;uint32_t _statusChange=0;
  Listener _server;
 private:Middleware middleware;
};
''')
 (d/'test.cpp').write_text(r'''
#include <cassert>
#include "station_http.h"
struct Clock{static uint32_t ms;static uint32_t nowUs(){return ms*1000;}static uint32_t nowMs(){return ms;}};
uint32_t Clock::ms=0;
int main(){
 station_http::Server<Clock> s;s.pending(100);Clock::ms=349;s.handleClient();assert(!s.stopped());
 Clock::ms=350;s.handleClient();assert(s.stopped()&&s.idleConnectionsClosed()==1);
 s.pending(100,1);Clock::ms=5000;s.handleClient();assert(!s.stopped()&&s.timing().requests==1);
 s.pending(100,0,HC_WAIT_CLOSE);s.handleClient();assert(!s.stopped());
 s.pending(0xfffffff0);Clock::ms=0xe9;s.handleClient();assert(!s.stopped());
 Clock::ms=0xea;s.handleClient();assert(s.stopped()&&s.idleConnectionsClosed()==2);
 s.pending(1000);Clock::ms=1010;s.queued(true);s.handleClient();
 assert(!s.stopped()&&s.idleConnectionsClosed()==2);
 Clock::ms=1100;s.handleClient();
 assert(s.stopped()&&s.idleConnectionsClosed()==3);
 s.pending(1000,1);s.handleClient();assert(!s.stopped()&&s.timing().requests==2);
 s.pending(1000,0,HC_WAIT_CLOSE);s.handleClient();assert(!s.stopped());
 assert(s.polls==10);
}
''')
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(d),'-I'+str(ROOT/'include'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS idle preconnect expiry at 250 ms or queued connection after 100 ms grace, request bytes/SSE untouched, timing middleware and millis wrap')
