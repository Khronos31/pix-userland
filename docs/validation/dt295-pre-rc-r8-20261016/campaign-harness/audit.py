"""Whole-file TS/sections/complete main A/V PES audit; no USB or payload output."""
import argparse,collections,hashlib,importlib.util,json,os,subprocess,sys
from pathlib import Path
P=Path(__file__).resolve().parent;ROOT=P.parents[2];TOOLS=ROOT/'work/pix-dt295-static'
sys.path.insert(0,str(TOOLS))
from inspect_si import Inventory,crc
spec=importlib.util.spec_from_file_location('av_audit',P/'audit_av_adts_v2.py');avmod=importlib.util.module_from_spec(spec);spec.loader.exec_module(avmod)
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(4*1024**2),b''):h.update(b)
 return h.hexdigest()
class Sections(Inventory):
 def __init__(self):
  super().__init__();self.checked=collections.Counter();self.failures=[];self.packet_number=0
 def section(self,pid,s):
  if len(s)<3:self.errors['short_section']+=1;return
  if s[1]&128 or s[0]==0x73:
   self.checked[(pid,s[0])]+=1
   if len(s)<4 or crc(s):self.failures.append({'pid':pid,'table_id':s[0],'packet':self.packet_number,'bytes':len(s)})
  super().section(pid,s)
  for pm in self.pmts:
   self.psi.update(es['pid'] for es in pm['streams'] if es['type'] in (5,13))
def audit(stage):
 os.umask(0o077);ts=stage/'capture.ts';r=json.loads((stage/'result.json').read_text());issues=[]
 def require(ok,why):
  if not ok:issues.append(why)
 size=ts.stat().st_size;n=size//188
 require(n>0 and size%188==0,'empty_or_partial_TS')
 with (stage/'integrity.json').open('x') as out,(stage/'audit.log').open('a') as log:
  code=subprocess.call([str(P/'inspect_ts'),str(ts),str(stage/'cc-events.jsonl'),str(n)],stdout=out,stderr=log)
 require(code in (0,1),'TS_checker_execution')
 integrity=json.loads((stage/'integrity.json').read_text())
 for k in ('tei','scrambled','cc_errors','invalid','pat_pmt_crc_errors'):require(integrity[k]==0,k)
 require(integrity['packets']==n and integrity['pat'] and integrity['pmt'],'whole_file_scan_and_tables')
 s=Sections()
 with ts.open('rb') as f:
  for block in iter(lambda:f.read(188*8192),b''):
   for off in range(0,len(block),188):s.packet_number+=1;s.packet(block[off:off+188])
 section=s.report();section.update(packets_examined=s.packet_number,all_crc_sections=sum(s.checked.values()),crc_failures=s.failures,checked=[{'pid':a,'table_id':b,'sections':v} for (a,b),v in sorted(s.checked.items())])
 (stage/'section-audit.json').write_text(json.dumps(section,indent=2)+'\n')
 require(not s.failures,'section_CRC');require(not s.errors,'section_structure_or_CC')
 pairs=set()
 for pm in s.pmts:
  vs=[x for x in pm['streams'] if x['type'] in (1,2,27,36)];aa=[x for x in pm['streams'] if x['type'] in (3,4,15,17)]
  if vs and aa:
   # Decode complete PES with ADTS reassembly and explicit initial PCE acquisition.
   require(vs[0]['type'] in (1,2,27) and aa[0]['type']==15,'unsupported_offline_codec')
   pairs.add((vs[0]['pid'],aa[0]['pid'],vs[0]['type']))
 require(bool(pairs),'no_main_AV_pair')
 av=[]
 for video,audio,video_type in sorted(pairs):
  label='av-'+str(video)+'-'+str(audio)
  x=avmod.audit(stage,video=video,audio=audio,label=label,packets=n,video_codec='h264' if video_type==27 else 'mpegvideo');av.append({'video':video,'audio':audio,'valid':x['valid'],'result':label+'/result.json'})
  require(x['valid'],'complete_main_AV_PES_decode')
 end=r.get('end') or {};status=r.get('final_status') or {}
 require(r.get('exit_code')==0,'receiver_exit')
 require(end.get('state')==16 and r.get('after_state',{}).get('firmware_state')==16,'final_state_16')
 require(not end.get('native_abandoned'),'native_abandoned')
 require(end.get('packets')==n and status.get('confirmed_bytes')==size,'packet_and_written_byte_accounting')
 require(end.get('elapsed_ms',0)>=r.get('minimum_seconds',0)*1000,'duration_short')
 for k in ('pending_bytes','rejected_bytes','uncertain_bytes','usb_error','usb_timeouts','writer_error','usb_active_transfers'):
  require(status.get(k)==0,k)
 require(status.get('usb_max_gap_ns',10**20)<10**9,'USB_gap_ge_1s')
 require(status.get('usb_delivered_bytes')==size+end.get('trailing_bytes',0)+end.get('stop_drained_bytes',0),'USB_delivered_vs_saved_and_stop_drain')
 digest=sha(ts);independent=subprocess.check_output(['shasum','-a','256',str(ts)],text=True).split()[0];require(digest==independent,'independent_file_SHA_read')
 result={'passed':not issues,'problems':issues,'packets':n,'bytes':size,'duration_seconds':end.get('elapsed_ms',0)/1000,'integrity':{k:v for k,v in integrity.items() if k!='pids'},'section_count':sum(s.checked.values()),'section_crc_failures':len(s.failures),'section_errors':dict(s.errors),'main_AV':av,'file_sha256':digest,'independent_file_read_sha_matches':digest==independent,'final_status':status,'end':end,'limitations':['Raw USB not saved; no independent full-input AES comparison','SHA compares two reads of saved TS; runtime writer has no independent stream SHA','Only complete advertised MPEG-2/H.264 video and main AAC PES decoded; capture-boundary PES omitted by the offline decoder','Cumulative native USB gap/queue peaks cover the run; status samples are about 10s apart','Advertised finite PID set plus SI; NULL/unknown unadvertised PIDs are not recorded']}
 (stage/'findings.json').write_text(json.dumps(result,indent=2)+'\n')
 return result
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('stage',type=Path);v=a.parse_args();r=audit(v.stage)
 print(json.dumps({k:r[k] for k in ('passed','problems','packets','duration_seconds')}));raise SystemExit(0 if r['passed'] else 1)
