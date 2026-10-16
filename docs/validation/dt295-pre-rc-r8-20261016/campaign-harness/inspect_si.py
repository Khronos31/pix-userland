"""Offline PSI/SI inventory with CRC checking. No ARIB text decoding or USB I/O.

Reads a frozen packet count even if the input recording is still growing.
Repeated identical sections are CRC checked once, counted on every appearance.
"""
import argparse, collections, json, os
from pathlib import Path

TABLE=[]
for i in range(256):
    v=i<<24
    for _ in range(8): v=((v<<1)^(0x04c11db7 if v&0x80000000 else 0))&0xffffffff
    TABLE.append(v)
def crc(b):
    v=0xffffffff
    for x in b: v=((v<<8)&0xffffffff)^TABLE[(v>>24)^x]
    return v
def u16(b,i): return (b[i]<<8)|b[i+1]

class Inventory:
    def __init__(self):
        self.buffers={}; self.previous={}; self.psi={0,1,0x10,0x11,0x12,0x14,0x23,0x24,0x25,0x26,0x27,0x28,0x29}
        self.tables=collections.Counter(); self.errors=collections.Counter();self.unique={}
        self.pats=[];self.pmts=[];self.services=set();self.events=set();self.eit_services=set()
    def section(self,pid,s):
        tid=s[0]; key=(pid,tid)
        self.tables[key]+=1
        if s in self.unique: return
        requires=bool(s[1]&128) or tid==0x73
        good=not requires or crc(s)==0
        self.unique[s]=good
        if not good: self.errors['crc']+=1;return
        if tid==0 and len(s)>=12:
            rows=[(u16(s,i),u16(s,i+2)&8191) for i in range(8,len(s)-4,4)]
            for program,pmt in rows:
                if program:self.psi.add(pmt)
            p={'tsid':u16(s,3),'version':(s[5]>>1)&31,'programs':rows}
            if p not in self.pats:self.pats.append(p)
        elif tid==2 and len(s)>=16:
            i=12+(u16(s,10)&4095);es=[]
            while i+5<=len(s)-4:
                es.append({'type':s[i],'pid':u16(s,i+1)&8191});i+=5+(u16(s,i+3)&4095)
            p={'pid':pid,'program':u16(s,3),'version':(s[5]>>1)&31,'pcr_pid':u16(s,8)&8191,'streams':es}
            if p not in self.pmts:self.pmts.append(p)
        elif tid in (0x42,0x46) and len(s)>=15:
            i=11
            while i+5<=len(s)-4:
                self.services.add((u16(s,8),u16(s,3),u16(s,i)));i+=5+(u16(s,i+3)&4095)
        elif 0x4e<=tid<=0x6f and len(s)>=18:
            sid=u16(s,3);tsid=u16(s,8);onid=u16(s,10)
            self.eit_services.add((onid,tsid,sid));i=14
            while i+12<=len(s)-4:
                self.events.add((onid,tsid,sid,u16(s,i),s[i+2:i+7].hex()))
                i+=12+(u16(s,i+10)&4095)
    def consume(self,pid,allow_new=True):
        b=self.buffers[pid]
        while len(b)>=3:
            if b[0]==255:b.clear();return
            n=3+(u16(b,1)&4095)
            if n>4096:self.errors['section_length']+=1;b.clear();return
            if len(b)<n:return
            s=bytes(b[:n]);del b[:n];self.section(pid,s)
            if not allow_new:b.clear();return
    def packet(self,p):
        pid=u16(p,1)&8191
        if pid not in self.psi:return
        af=(p[3]>>4)&3
        if not af or (af&2 and p[4]>183):self.errors['invalid_packet']+=1;return
        if p[1]&128:self.buffers.pop(pid,None);self.previous.pop(pid,None);self.errors['tei']+=1;return
        if af&2 and p[4] and p[5]&128:self.buffers.pop(pid,None);self.previous.pop(pid,None)
        if not af&1:return
        cc=p[3]&15
        if pid in self.previous:
            old=self.previous[pid]
            if old==p:return
            if cc!=((old[3]+1)&15):self.buffers.pop(pid,None);self.errors['si_cc_gap']+=1
        self.previous[pid]=p
        start=5+p[4] if af&2 else 4
        if start>=188:return
        b=p[start:]
        if p[1]&64:
            pointer=b[0]
            if pointer+1>len(b):self.errors['pointer']+=1;self.buffers.pop(pid,None);return
            if pid in self.buffers:
                self.buffers[pid]+=b[1:pointer+1];self.consume(pid,False)
            self.buffers[pid]=bytearray(b[pointer+1:]);self.consume(pid)
        elif pid in self.buffers:
            self.buffers[pid]+=b;self.consume(pid,False)
    def report(self):
        return {'tables':[{ 'pid':p,'table_id':hex(t),'sections':n} for (p,t),n in sorted(self.tables.items())],
                'unique_sections':len(self.unique),'unique_crc_failures':sum(not x for x in self.unique.values()),
                'errors':dict(self.errors),'pat':self.pats,'pmt':self.pmts,'sdt_services':sorted(self.services),
                'eit_services':sorted(self.eit_services),'unique_events':len(self.events)}

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('input',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--packets',type=int)
    a=ap.parse_args();os.umask(0o077);limit=a.packets or a.input.stat().st_size//188;i=Inventory();n=0
    with a.input.open('rb') as f:
        while n<limit:
            data=f.read(min(32768,limit-n)*188)
            if not data:break
            for off in range(0,len(data)//188*188,188):i.packet(data[off:off+188]);n+=1
    report=i.report();report.update(packets=n,requested_packets=limit)
    with a.output.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps({'packets':n,'unique_sections':report['unique_sections'],'errors':report['errors'],'services':len(i.services),'events':len(i.events)}))
if __name__=='__main__':main()
