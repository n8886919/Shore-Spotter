#!/usr/bin/env python3
"""Read-only T096 USB diagnostics; exact USB identity, no reset or flash.
Use the PlatformIO Python (pyserial installed). Private output may contain GPS.
"""
import argparse,json,os,time


def fields(line):
    return dict(item.split('=',1) for item in line.split()[2:] if '=' in item)


class NmeaExport:
    def __init__(self):
        self.header=None;self.records=[];self.done=False
    def accept(self,line):
        if not line.startswith('@NMEA '):return
        if self.done:raise ValueError('data after END')
        f=fields(line)
        if line.startswith('@NMEA BEGIN '):
            if self.header is not None:raise ValueError('duplicate BEGIN')
            count=int(f['count']);total=int(f['total']);overwritten=int(f['overwritten'])
            if not 0<=count<=4 or total-count!=overwritten:raise ValueError('invalid counts')
            self.header={**f,'count':count,'total':total,'overwritten':overwritten}
            return
        if self.header is None:raise ValueError('missing BEGIN')
        if line.startswith('@NMEA META '):
            if self.records and len(self.records[-1]['raw_hex'])!=2*self.records[-1]['length']:raise ValueError('short previous record')
            length=int(f['len']);ident=int(f['id'])
            if not 0<=length<=160 or len(self.records)>=self.header['count']:raise ValueError('oversize record')
            if ident!=self.header['total']-self.header['count']+len(self.records)+1:raise ValueError('unexpected record id')
            self.records.append({'id':ident,'ms':int(f['ms']),'reason':f['reason'],'length':length,'truncated':bool(int(f['truncated'])),'raw_hex':''})
        elif line.startswith('@NMEA DATA '):
            if not self.records:raise ValueError('DATA without META')
            r=self.records[-1];data=bytes.fromhex(f['hex'])
            if int(f['id'])!=r['id'] or int(f['offset'])!=len(r['raw_hex'])//2 or len(r['raw_hex'])//2+len(data)>r['length']:raise ValueError('invalid DATA offset/length/id')
            r['raw_hex']+=data.hex()
        elif line.startswith('@NMEA END '):
            if int(f['count'])!=self.header['count'] or len(self.records)!=self.header['count']:raise ValueError('missing records')
            if any(len(r['raw_hex'])!=2*r['length'] for r in self.records):raise ValueError('truncated transfer')
            self.done=True
        else:raise ValueError('unknown NMEA response')


def main():
    import serial
    from serial.tools import list_ports
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True,help='exact USB serial, not ttyACM number')
    p.add_argument('--out',required=True,help='new private JSON path (never overwritten)')
    a=p.parse_args();ports=[x for x in list_ports.comports() if x.serial_number==a.serial]
    if len(ports)!=1:raise SystemExit('Expected exactly one matching USB serial')
    if os.path.exists(a.out):raise SystemExit('Output already exists')
    s=serial.Serial(port=None,baudrate=115200,timeout=.3,exclusive=True)
    s.dtr=True;s.rts=False;s.port=ports[0].device;s.open()
    log=[];status=None;export=NmeaExport()
    try:
        s.write(b'DIAG STATUS\n')
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and status is None:
            raw=s.readline();log.append(raw.hex());line=raw.decode('ascii','replace').strip()
            if line.startswith('@DIAG boot='):status=dict(x.split('=',1) for x in line.split()[1:])
        if status is None:raise RuntimeError('No DIAG STATUS response')
        s.write(b'DIAG NMEA\n');deadline=time.monotonic()+15
        while time.monotonic()<deadline and not export.done:
            raw=s.readline();log.append(raw.hex());export.accept(raw.decode('ascii','replace').strip())
        if not export.done:raise RuntimeError('Incomplete NMEA export')
        if status['boot']!=export.header['boot']:raise RuntimeError('boot changed during export')
        result={'usb_serial':a.serial,'captured_unix':time.time(),'status':status,'nmea':{'header':export.header,'records':export.records},'usb_raw_chunks_hex':log}
        with os.fdopen(os.open(a.out,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600),'w') as f:json.dump(result,f,ensure_ascii=False,indent=2)
        print(json.dumps({'status':status,'nmea_records':len(export.records),'saved_to':a.out}))
    finally:s.close()


if __name__=='__main__':main()
