#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include "gnss_snapshot.h"

// Diagnostic-only observation. Never changes the production GNSS collector.
namespace field_diagnostic {
constexpr uint32_t kPhaseMs = 180000;
class Plan {
 public:
  bool observe(uint32_t now, bool usable) {
    if (phase_ == 0) {
      if (usable) {
        if (!haveGood_ || now-lastGood_ > 2000) firstGood_=now;
        haveGood_=true; lastGood_=now;
        if (now-firstGood_ >= 30000) { phase_=1; started_=now; return true; }
      } else if (haveGood_ && now-lastGood_ > 2000) haveGood_=false;
    } else if (phase_ < 5 && now-started_ >= kPhaseMs) {
      ++phase_; started_=now; return true;
    }
    return false;
  }
  uint8_t phase() const { return phase_; }
  bool rf() const { return phase_ != 0 && phase_ != 1; }
  void abort() { phase_=6; }
  bool sd() const { return phase_ == 3; }
 private:
  uint8_t phase_=0;
  bool haveGood_=false;
  uint32_t firstGood_=0,lastGood_=0,started_=0;
};

// Accept a date ONLY from a checksummed RMC with a valid time/date. A sample's
// UTC date is usable only when that exact RMC epoch matches, including midnight.
class Utc {
 public:
  void invalidate() { valid_=false; collecting_=false; length_=0; }
  void feed(char c,uint32_t now) {
    if(c=='$') { length_=0; collecting_=true; }
    if(!collecting_) return;
    if(c=='\n') { line_[length_]=0; collecting_=false; consume(now); return; }
    if(c=='\r') return;
    if(length_==sizeof(line_)-1) { collecting_=false; return; }
    line_[length_++]=c;
  }
  bool matches(uint32_t epoch,uint32_t now) const { return valid_ && epoch==epoch_ && now-ms_<2000; }
  uint32_t date() const { return date_; }
  uint32_t age(uint32_t now) const { return valid_ ? now-ms_ : UINT32_MAX; }
 private:
  bool valid_=false,collecting_=false;
  char line_[192]{}; size_t length_=0;
  uint32_t date_=0,epoch_=0,ms_=0;
  static int hex(char c) { return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1; }
  static bool digits(const char*s,size_t n) { for(size_t i=0;i<n;++i) if(s[i]<'0'||s[i]>'9')return false; return true; }
  static unsigned pair(const char*s) { return (s[0]-'0')*10+s[1]-'0'; }
  void consume(uint32_t now) {
    char *star=strchr(line_,'*'); if(!star||strlen(star)!=3) return;
    uint8_t sum=0; for(char*p=line_+1;p<star;++p)sum^=uint8_t(*p);
    const int a=hex(star[1]),b=hex(star[2]); if(a<0||b<0||sum!=(a*16+b))return;
    *star=0; char* f[16]{}; size_t count=1;f[0]=line_+1;
    for(char*p=line_+1;*p&&count<16;++p)if(*p==','){*p=0;f[count++]=p+1;}
    if(strlen(f[0])!=5||strcmp(f[0]+2,"RMC"))return;
    valid_=false; // an invalid new RMC must not borrow an older valid date
    if(count<10)return;
    if(strlen(f[1])<6||!digits(f[1],6)||strlen(f[9])!=6||!digits(f[9],6))return;
    unsigned h=pair(f[1]),m=pair(f[1]+2),s=pair(f[1]+4),ms=0;
    if(h>23||m>59||s>59)return;
    if(f[1][6]) {
      if(f[1][6]!='.'||!f[1][7]||strlen(f[1])>10)return;
      unsigned scale=100;
      for(const char*p=f[1]+7;*p;++p) { if(*p<'0'||*p>'9')return; if(scale){ms+=(*p-'0')*scale;scale/=10;} }
    }
    unsigned d=pair(f[9]),mon=pair(f[9]+2),y=2000+pair(f[9]+4);
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(mon<1||mon>12||d<1||d>unsigned(days[mon-1]+(mon==2&&y%4==0)))return;
    date_=y*10000+mon*100+d; epoch_=((h*60+m)*60+s)*1000+ms;ms_=now;valid_=true;
  }
};

struct Metrics {
  uint8_t phase=0;
  bool rf=true,sd=false;
  uint16_t battery=0;
  uint32_t heap=0,backlog=0,tx=0,txErrors=0,rawSplits=0,loopGap=0;
};
inline void put(uint8_t*& p,uint64_t value,unsigned n) { while(n--) {*p++=uint8_t(value);value>>=8;} }
inline void number(uint8_t*& p,double value) { uint64_t bits;static_assert(sizeof(value)==8,"IEEE double");memcpy(&bits,&value,8);put(p,bits,8); }
inline void number(uint8_t*& p,float value) { uint32_t bits;static_assert(sizeof(value)==4,"IEEE float");memcpy(&bits,&value,4);put(p,bits,4); }
// Stable little-endian 80-byte snapshot v1; full doubles preserve parser output.
inline void encode(uint8_t out[80],const gnss_snapshot::Collector& collector,
                   const Utc& utc,uint32_t now,const Metrics& m) {
  gnss_snapshot::Snapshot s; const bool have=collector.sample(now,s);
  const bool timeOk=have&&s.haveRmc&&utc.matches(s.epochMsOfDay,now);
  uint8_t*p=out;
  put(p,1,1);put(p,(have?1:0)|(s.fix?2:0)|(s.haveRmc?4:0)|(s.haveGga?8:0)|
      (timeOk?16:0)|(collector.recovering()?32:0)|(m.rf?64:0)|(m.sd?128:0),1);
  put(p,s.satellites,1);put(p,m.phase,1);
  put(p,s.epochMsOfDay,4);put(p,s.sourceAgeMs,4);put(p,s.arrivalAgeMs,4);put(p,timeOk?utc.date():0,4);
  number(p,s.lat);number(p,s.lon);number(p,float(s.speedMps));number(p,float(s.courseDeg));number(p,float(s.hdop));
  put(p,m.battery,2);put(p,0,2);
  for(uint32_t v:{m.heap,m.backlog,m.tx,m.txErrors,m.rawSplits,m.loopGap,utc.age(now)})put(p,v,4);
}
}
