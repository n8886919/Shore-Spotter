#pragma once
#include "gnss_snapshot.h"
#include <string.h>
#include <stdio.h>

// Evidence only: no parser decisions, RF transmission or flash writes.
namespace nmea_evidence {
constexpr size_t kCapacity = 4, kRawCapacity = 160;
struct Record {
  uint32_t id = 0, ms = 0;
  gnss_snapshot::RejectReason reason = gnss_snapshot::RejectReason::None;
  uint16_t length = 0;
  bool truncated = false;
  uint8_t raw[kRawCapacity]{};
};
class Store {
 public:
  void feed(gnss_snapshot::Collector &parser, char ch, uint32_t now) {
    if(ch == '$') {
      if(active_) save(gnss_snapshot::RejectReason::Resync, now, true);
      active_ = true; length_ = 0;
    }
    if(active_ && length_ < kRawCapacity) raw_[length_++] = uint8_t(ch);
    const uint32_t before = parser.stats().rejectedSentences;
    parser.feed(ch, now);
    if(parser.stats().rejectedSentences != before) {
      save(parser.lastRejectReason(), now, ch != '\n');
      active_ = false; length_ = 0;
    } else if(ch == '\n') { active_ = false; length_ = 0; }
  }
  void interrupted(uint32_t now) {
    if(active_) save(gnss_snapshot::RejectReason::Backlog, now, true);
    active_ = false; length_ = 0;
  }
  uint32_t total() const { return total_; }
  uint32_t overwritten() const { return total_ > kCapacity ? total_ - kCapacity : 0; }
  size_t count() const { return total_ < kCapacity ? total_ : kCapacity; }
  const Record &at(size_t oldestIndex) const {
    return records_[(total_ - count() + oldestIndex) % kCapacity];
  }
 private:
  void save(gnss_snapshot::RejectReason reason, uint32_t now, bool truncated) {
    Record &r = records_[total_ % kCapacity];
    r.id = ++total_; r.ms = now; r.reason = reason; r.length = length_;
    r.truncated = truncated; memcpy(r.raw, raw_, length_);
  }
  Record records_[kCapacity]{};
  uint8_t raw_[kRawCapacity]{};
  size_t length_ = 0;
  uint32_t total_ = 0;
  bool active_ = false;
};
// Immutable snapshot, one small line at a time; caller retains a line until
// USB has room. Hex preserves commas, CR/LF, control bytes and malformed text.
class Export {
 public:
  void begin(const Store &store, uint32_t boot) {
    count_ = store.count(); total_ = store.total(); boot_ = boot;
    for(size_t i=0;i<count_;++i) records_[i]=store.at(i);
    stage_=1; index_=offset_=0;
  }
  bool busy() const { return stage_ != 0; }
  bool next(char *out, size_t capacity) {
    if(!stage_) return false;
    if(stage_==1) {
      snprintf(out,capacity,"@NMEA BEGIN boot=%08lX count=%u total=%lu overwritten=%lu\n",
        (unsigned long)boot_,unsigned(count_),(unsigned long)total_,
        (unsigned long)(total_-count_)); stage_=2; return true;
    }
    if(index_==count_) {
      snprintf(out,capacity,"@NMEA END count=%u\n",unsigned(count_)); stage_=0; return true;
    }
    const auto &r=records_[index_];
    if(stage_==2) {
      snprintf(out,capacity,"@NMEA META id=%lu ms=%lu reason=%s len=%u truncated=%u\n",
        (unsigned long)r.id,(unsigned long)r.ms,gnss_snapshot::rejectName(r.reason),r.length,r.truncated);
      stage_=3; offset_=0; return true;
    }
    char hex[49]; size_t n=r.length-offset_; if(n>24)n=24;
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;++i) { uint8_t b=r.raw[offset_+i]; hex[2*i]=digits[b>>4];hex[2*i+1]=digits[b&15]; }
    hex[2*n]=0;
    snprintf(out,capacity,"@NMEA DATA id=%lu offset=%u hex=%s\n",(unsigned long)r.id,unsigned(offset_),hex);
    offset_+=n; if(offset_==r.length) { ++index_;stage_=2; }
    return true;
  }
 private:
  Record records_[kCapacity]{};
  size_t count_=0,index_=0,offset_=0;
  uint32_t total_=0,boot_=0;
  uint8_t stage_=0;
};
} // namespace nmea_evidence
