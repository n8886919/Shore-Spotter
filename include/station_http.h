#pragma once
#include <WebServer.h>
#include "http_timing.h"

namespace station_http {
// Browsers may preconnect without a request. Arduino WebServer otherwise
// reserves its single active client for 5 s, delaying every other API caller
// even though the firmware loop/RF remain responsive. Only expire connections
// that have not supplied any bytes. Give a new connection a short grace for
// its first bytes, then yield if another connection is queued rather than
// making it wait for the full idle timer (or TCP retry).
// NetworkServer::hasClient() is non-blocking and retains that accepted socket
// for the next accept(). Do not cut off a parsed request or upload.
constexpr uint32_t kIdleRequestMs=250;
constexpr uint32_t kQueuedIdleRequestMs=100;
template<class Clock>
class Server : public http_timing::Server<WebServer,Clock> {
 public:
  explicit Server(int port=80):http_timing::Server<WebServer,Clock>(port){}
  void handleClient() override {
    const uint32_t idleMs=uint32_t(Clock::nowMs()-this->_statusChange);
    if(this->_currentStatus==HC_WAIT_READ && !this->_currentClient.available() &&
        (idleMs>=kIdleRequestMs ||
         (idleMs>=kQueuedIdleRequestMs && this->_server.hasClient()))) {
      this->_currentClient.stop();
      this->_currentStatus=HC_NONE;
      ++idleClosed_;
    }
    http_timing::Server<WebServer,Clock>::handleClient();
  }
  uint32_t idleConnectionsClosed()const{return idleClosed_;}
 private:
  uint32_t idleClosed_=0;
};
}
