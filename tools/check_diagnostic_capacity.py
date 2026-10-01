#!/usr/bin/env python3
"""Model diagnostic flash capacity using the actual C++ codec, without a device.

This estimates byte/frame demand, not measured RF timing or storage latency.
Inputs are explicit: 1 Hz RMC/GGA and 80-byte snapshots; two 21-byte TX event
records per second; a 1500-byte SD JSON status split at 400 bytes and a
140-byte recorder-health record every 10 s.
The conservative profile uses two full 128-byte raw chunks every second.
The worker's 1000 ms partial-frame flush is included, with a 5 ms service tick.
"""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def estimate():
    source = r'''
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>
#include "diagnostic_store_codec.h"
using namespace diagnostic_store::codec;
struct Event { uint32_t ms; uint16_t kind, length; };
struct Result { uint32_t frames, records; uint64_t payload; uint32_t firstFullMs; };
Result simulate(uint32_t seconds, uint16_t rawLength, bool fullRf) {
  std::vector<Event> events;
  // Two explicit startup metadata records and one phase record.
  events.push_back({0,2,200}); events.push_back({0,2,110}); events.push_back({0,4,100});
  for (uint32_t second=0; second<seconds; ++second) {
    const uint32_t ms=second*1000;
    events.push_back({ms,3,80});
    if (second%10==0) {
      events.push_back({ms,5,140});
      for (unsigned length : {400U,400U,400U,300U,1U})
        events.push_back({ms,8,uint16_t(length)});
    }
    events.push_back({ms+100,1,rawLength});
    events.push_back({ms+110,1,rawLength});
    // Plan spends its first 30 s warming up plus 180 s in RF-off baseline.
    if (fullRf || second>=210) {
      events.push_back({ms+130,7,21});
      events.push_back({ms+490,7,21});
    }
    if (second>=30 && (second-30)%180==0 && second<=750)
      events.push_back({ms,4,100});
  }
  std::stable_sort(events.begin(),events.end(),[](const Event&a,const Event&b){return a.ms<b.ms;});
  uint8_t frame[kFrameBytes], payload[kMaxRecordBytes]{};
  Result result{1,0,0,0}; bool buffered=false; uint32_t began=0;
  const auto flush=[&](uint32_t now) {
    if(!buffered)return;
    seal(frame); if(!validFrame(frame))std::abort();
    ++result.frames; buffered=false;
    if(result.frames==kPartitionBytes/kFrameBytes && !result.firstFullMs)result.firstFullMs=now;
  };
  size_t next=0;
  for(uint32_t now=0;now<=seconds*1000+1000;now+=5) {
    while(next<events.size() && events[next].ms<=now) {
      const Event e=events[next++];
      if(buffered && !append(frame,e.kind,payload,e.length,e.ms))flush(now);
      else if(buffered) {
        ++result.records;result.payload+=e.length;
        if(get16(frame+8)==kPayloadBytes)flush(now);
        continue;
      }
      if(!buffered) {beginFrame(frame,42,result.frames-1,e.ms);buffered=true;began=now;}
      if(!append(frame,e.kind,payload,e.length,e.ms))std::abort();
      ++result.records;result.payload+=e.length;
      if(get16(frame+8)==kPayloadBytes)flush(now);
    }
    if(buffered && now-began>=1000)flush(now);
  }
  return result;
}
int main() {
  for(bool fullRf : {false,true})for(uint16_t rawLength : {80,128})for(uint32_t seconds : {1200,1800,3600}) {
    const auto r=simulate(seconds,rawLength,fullRf);
    std::cout << seconds << ' ' << rawLength << ' ' << fullRf << ' ' << r.frames << ' '
              << r.records << ' ' << r.payload << ' ' << r.firstFullMs << '\n';
  }
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path / 'capacity.cpp').write_text(source)
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'include'), str(path / 'capacity.cpp'),
                        '-o', str(path / 'capacity')], check=True)
        output = subprocess.check_output([str(path / 'capacity')], text=True)
    rows = []
    for line in output.splitlines():
        seconds, raw_length, full_rf, frames, records, payload, full_ms = map(int, line.split())
        rows.append({'duration_minutes': seconds // 60,
                     'profile': 'rf_always_on' if full_rf else 'planned_rf_off_first_210s',
                     'raw_bytes_per_second': raw_length * 2,
                     'required_frames': frames, 'required_flash_bytes': frames * 512,
                     'capacity_frames': 3072, 'capacity_percent': round(frames / 3072 * 100, 2),
                     'records': records, 'payload_bytes': payload,
                     'first_full_minutes': round(full_ms / 60000, 2) if full_ms else None})
    return rows


if __name__ == '__main__':
    rows = estimate()
    fits_thirty = all(row['required_frames'] < row['capacity_frames']
                      for row in rows if row['duration_minutes'] == 30)
    exceeds_sixty = all(row['required_frames'] > row['capacity_frames']
                        for row in rows if row['duration_minutes'] == 60)
    print(json.dumps({'assumptions': __doc__, 'all_modeled_30min_profiles_fit': fits_thirty,
                      'all_modeled_60min_profiles_exceed_capacity': exceeds_sixty,
                      'estimates': rows}, indent=2))
    raise SystemExit(0 if fits_thirty and exceeds_sixty else 1)
