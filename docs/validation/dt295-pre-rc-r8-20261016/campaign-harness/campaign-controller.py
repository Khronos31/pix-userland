"""One-shot r8 development validation campaign. Do not invoke directly."""
import fcntl, hashlib, json, os, shutil, signal, subprocess, sys, time
from pathlib import Path
P=Path(__file__).resolve().parent; ROOT=P.parents[2]
BIN=P/'development-r8/pix-ts'; SOURCE=P/'development-r8/source'
DATA=Path('/Volumes/External/pix-dt295-pre-rc-r8-20261016-01')
CFG=P/'campaign-config.json'; MARK=P/'start-once.json'; PREFLIGHT=P/'preflight.json'

def write(path,data):
    t=path.with_suffix(path.suffix+'.tmp');t.write_text(json.dumps(data,indent=2)+'\n');t.replace(path)
def write_lines(path,rows):
    t=path.with_suffix(path.suffix+'.tmp');t.write_text(''.join(json.dumps(x,separators=(',',':'))+'\n' for x in rows));t.replace(path)
def event(kind,**data):
    with (P/'campaign-controller.log').open('a') as f:f.write(json.dumps(dict(time_unix=time.time(),event=kind,**data))+'\n')
def cleanup_event(kind,**data):
    try:event(kind,**data)
    except OSError:pass
def state(phase,**data):write(P/'campaign-state.json',dict(phase=phase,updated_unix=time.time(),**data))
def probe():
    r=subprocess.run([sys.executable,str(P/'read_state.py')],text=True,capture_output=True,timeout=10);r.check_returncode();return json.loads(r.stdout)
def members(pgid):
    rows=subprocess.check_output(['ps','-axo','pid=,pgid=,stat=,command='],text=True).splitlines();out=[]
    for row in rows:
        q=row.strip().split(None,3)
        if len(q)==4 and q[1].isdigit() and int(q[1])==pgid:out.append({'pid':int(q[0]),'state':q[2],'command':q[3]})
    return out
def owned(pid,capture):
    cmd=subprocess.run(['ps','-p',str(pid),'-o','command='],text=True,capture_output=True).stdout
    return str(BIN) in cmd and str(capture) in cmd
def audit_command(pid):return subprocess.run(['ps','-p',str(pid),'-o','command='],text=True,capture_output=True).stdout.strip()
def owns_audit(pid,stage,script):
    cmd=audit_command(pid);words=cmd.split()
    return bool(words and str(script) in cmd and str(stage) in cmd and Path(words[0]).name.lower().startswith('python'))
def expected_audit_member(row,script,stage):
    cmd=row['command'];words=cmd.split()
    if not words:return False
    if str(script) in cmd and str(stage) in cmd and 'python' in words[0].lower():return True
    exe=Path(words[0]).name
    if exe=='inspect_ts':return str(P/'inspect_ts') in words[0]
    return exe in ('ffmpeg','shasum')
def terminate_audit(proc,stage,script,why):
    root_ok=owns_audit(proc.pid,stage,script);current=members(proc.pid);live=[x for x in current if 'Z' not in x['state']]
    verified=[x for x in live if expected_audit_member(x,script,stage)];unknown=[x for x in live if x not in verified]
    cleanup_event('audit_cleanup',pid=proc.pid,reason=why,root_command_matches=root_ok,verified_members=verified,unverified_members=unknown)
    if proc.poll() is None and root_ok:
        if live and not unknown and any(x['pid']==proc.pid for x in verified):os.killpg(proc.pid,signal.SIGTERM)
        else:
            for x in verified:
                try:os.kill(x['pid'],signal.SIGTERM)
                except ProcessLookupError:pass
    until=time.monotonic()+10
    while time.monotonic()<until:
        try:proc.wait(timeout=.1)
        except subprocess.TimeoutExpired:pass
        live=[x for x in members(proc.pid) if 'Z' not in x['state']]
        if not live and proc.poll() is not None:break
        time.sleep(.1)
    live=[x for x in members(proc.pid) if 'Z' not in x['state']];verified=[x for x in live if expected_audit_member(x,script,stage)];unknown=[x for x in live if x not in verified]
    if live and not unknown:os.killpg(proc.pid,signal.SIGKILL)
    else:
        for x in verified:
            try:os.kill(x['pid'],signal.SIGKILL)
            except ProcessLookupError:pass
    try:proc.wait(timeout=10)
    except subprocess.TimeoutExpired:raise RuntimeError('audit parent did not exit after validated cleanup')
    until=time.monotonic()+3
    while True:
        survivors=[x for x in members(proc.pid) if 'Z' not in x['state']]
        if not survivors or time.monotonic()>=until:break
        time.sleep(.1)
    if survivors:raise RuntimeError('audit process group still has members after cleanup')
    if not root_ok:raise RuntimeError('audit PID command mismatch; process was not signaled')
def lock_free():
    with open('/tmp/pix-userland-dt295-'+str(os.getuid())+'.lock','a+') as f:
        try:fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB);fcntl.lockf(f,fcntl.LOCK_UN);return True
        except BlockingIOError:return False
def terminate(proc,capture,why):
    matches=owned(proc.pid,capture);cleanup_event('safe_stop',pid=proc.pid,reason=why,command_matches=matches)
    if proc.poll() is None and matches:proc.terminate()
    try:proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        rows=members(proc.pid)
        if rows and all(str(BIN) in x['command'] for x in rows):os.killpg(proc.pid,signal.SIGKILL)
        proc.wait(timeout=10)
def close_receiver_group(pid,capture):
    rows=members(pid);live=[x for x in rows if 'Z' not in x['state']]
    if not live:return {'safe_to_inspect':True,'had_survivors':False,'terminated':[],'remaining':[],'zombies':[x for x in rows if 'Z' in x['state']]}
    verified=[x for x in live if str(BIN) in x['command'] and str(capture) in x['command']]
    unknown=[x for x in live if x not in verified]
    if unknown:return {'safe_to_inspect':False,'had_survivors':True,'terminated':[],'remaining':live,'unverified':unknown}
    cleanup_event('receiver_group_cleanup',pid=pid,reason='receiver root exited with owned group members',verified_members=verified)
    try:os.killpg(pid,signal.SIGTERM)
    except ProcessLookupError:pass
    until=time.monotonic()+10
    while time.monotonic()<until:
        live=[x for x in members(pid) if 'Z' not in x['state']]
        if not live:break
        time.sleep(.1)
    if live:
        current=members(pid);live=[x for x in current if 'Z' not in x['state']]
        if all(str(BIN) in x['command'] and str(capture) in x['command'] for x in live):
            try:os.killpg(pid,signal.SIGKILL)
            except ProcessLookupError:pass
    until=time.monotonic()+5
    while time.monotonic()<until:
        live=[x for x in members(pid) if 'Z' not in x['state']]
        if not live:break
        time.sleep(.1)
    return {'safe_to_inspect':not live,'had_survivors':True,'terminated':verified,'remaining':live,'zombies':[x for x in members(pid) if 'Z' in x['state']]}
def fingerprint():
    m=json.loads((P/'source-hashes.json').read_text());files={x['path']:x['sha256'] for x in m['files']}
    actual={k:hashlib.sha256((SOURCE/k).read_bytes()).hexdigest() for k in files}
    if actual!=files:raise RuntimeError('frozen source file hash mismatch')
    return hashlib.sha256(''.join(f'{k} {files[k]}\n' for k in sorted(files)).encode()).hexdigest()
def verify_helpers():
    expected=json.loads((P/'audit-input-hashes.json').read_text())['files']
    for name,digest in expected.items():
        if hashlib.sha256((P/name).read_bytes()).hexdigest()!=digest:raise RuntimeError('copied audit helper hash mismatch: '+name)
    if hashlib.sha256((P/'inspect_ts').read_bytes()).hexdigest()!=expected['inspect_ts']:raise RuntimeError('inspect_ts hash mismatch')
def preflight():
    f=json.loads(PREFLIGHT.read_text())
    for key in ('software_cross_validation_passed','hardware_launch_authorized','source_binary_harness_verified'):
        if f.get(key) is not True:raise RuntimeError('preflight gate is false or absent: '+key)
    if f.get('binary_sha256')!=hashlib.sha256(BIN.read_bytes()).hexdigest():raise RuntimeError('preflight binary hash mismatch')
    return f
def verify_harness_manifest(pre):
    manifest=P/'campaign-harness-hashes.json';raw=manifest.read_bytes()
    if pre.get('harness_manifest_sha256')!=hashlib.sha256(raw).hexdigest():raise RuntimeError('preflight does not pin this harness manifest')
    m=json.loads(raw);files=m.get('files')
    if not isinstance(files,dict):raise RuntimeError('campaign harness manifest is malformed')
    required={'campaign-controller.py','campaign-config.json','start_once.py','live-add-evidence-review.py','audit.py','audit_av_adts_v2.py','read_state.py','inspect_ts'}
    if not required.issubset(files):raise RuntimeError('campaign harness manifest omits a runtime helper')
    for name,digest in files.items():
        path=Path(name);path=path if path.is_absolute() else P/path
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('campaign harness file hash mismatch: '+name)
    si=json.loads((P/'audit-input-hashes.json').read_text())['inspect_si']['path']
    if si not in files:raise RuntimeError('campaign harness manifest omits absolute inspect_si path')
def extract_diagnostics(stage):
    p=stage/'diagnostic/status.jsonl';rows=[];errors=[]
    if not p.exists():errors.append('status.jsonl missing')
    elif p.stat().st_size>64*1024*1024:errors.append('status.jsonl exceeds 64 MiB bound')
    else:
        with p.open(encoding='utf-8') as f:
            for line_no,line in enumerate(f,1):
                if not line.strip():continue
                try:obj=json.loads(line)
                except (json.JSONDecodeError,UnicodeError):errors.append('invalid JSON line '+str(line_no));continue
                if isinstance(obj,dict):rows.append(obj)
    statuses=[x for x in rows if x.get('model')=='DT295' and 'confirmed_bytes' in x]
    events=[x for x in rows if isinstance(x.get('event'),str)]
    live=[x for x in events if x.get('event')=='dt295_live_pid_add']
    write_lines(stage/'status-samples.jsonl',statuses)
    write_lines(stage/'all-events.jsonl',events)
    write_lines(stage/'live-add-events.jsonl',live)
    ends=[x for x in events if x.get('event')=='end']
    write(stage/'diagnostic-extraction.json',{'valid_rows':len(rows),'error_count':len(errors),'errors':errors[:100]})
    return (statuses[-1] if statuses else None,ends[-1] if ends else None,live,rows,errors)
def one(item,cfg):
    name=item['name'];seconds=item['seconds'];s=P/'stages'/name;s.mkdir(mode=0o700,parents=True,exist_ok=False)
    external=DATA/name;external.mkdir(mode=0o700,parents=True,exist_ok=False);capture=external/'capture.ts';(s/'capture.ts').symlink_to(capture)
    before=probe();write(s/'before-state.json',before)
    if before.get('firmware_state')!=16 or before.get('state_notifications_pending') is not False or not lock_free():raise RuntimeError('device is not idle16/notification-free/unlocked; physical review required')
    minimum=cfg['external_five_hour_minimum_free_bytes'] if item['seconds']>=18000 else cfg['external_runtime_minimum_free_bytes']
    if shutil.disk_usage(DATA).free<minimum:raise RuntimeError('external storage floor unavailable before stage')
    diag=s/'diagnostic';cmd=[str(BIN),'--model','dt295','--assets',str(P/'assets'),'--device','2-3','-c','T27','-t',str(seconds),'-o',str(capture),'--diagnostic',str(diag)]
    deadline=seconds+cfg['receiver_deadline_slack_seconds'];state('receiving',stage=name,requested_seconds=seconds,deadline_seconds=deadline)
    with (s/'console.log').open('xb') as log:
        proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=log,start_new_session=True)
        started=time.monotonic();last_space=0;forced=None
        try:
            write(s/'launch.json',{'pid':proc.pid,'command':cmd,'started_unix':time.time(),'deadline_seconds':deadline})
            event('receiver_started',stage=name,pid=proc.pid,seconds=seconds)
            while proc.poll() is None:
                now=time.monotonic()
                if now-last_space>=10:
                    last_space=now
                    if shutil.disk_usage(P).free<cfg['internal_minimum_free_bytes'] or shutil.disk_usage(DATA).free<cfg['external_runtime_minimum_free_bytes']:forced='storage floor'
                if now-started>deadline:forced='receiver deadline'
                if forced:terminate(proc,capture,forced);break
                time.sleep(.2)
            code=proc.wait()
        except BaseException as exc:
            try:terminate(proc,capture,'controller exception: '+repr(exc))
            except BaseException as ce:cleanup_event('receiver_exception_cleanup_failed',pid=proc.pid,error=repr(ce))
            raise
    group_cleanup=close_receiver_group(proc.pid,capture)
    write(s/'receiver-group-cleanup.json',group_cleanup)
    if not group_cleanup['safe_to_inspect']:
        result={'exit_code':code,'requested_seconds':seconds,'minimum_seconds':seconds,'wall_seconds':time.monotonic()-started,'before_state':before,'after_state':None,'remaining_processes':group_cleanup['remaining'],'device_lock_released':lock_free(),'final_status':None,'end':None,'forced_stop':forced,'live_add_event_count':None,'capture_closed':False}
        write(s/'result.json',result)
        write(s/'audit-not-run.json',{'reason':'unverified or surviving process may still mutate capture.ts; capture was not audited','unverified':group_cleanup.get('unverified',[]),'remaining':group_cleanup['remaining']})
        write(s/'stage-gate-results.json',{'passed':False,'failures':['capture writer process group did not close safely'],'audit_exit_code':None})
        raise RuntimeError('capture may still be mutable; stopped before device probe/diagnostic parsing/audit')
    time.sleep(.3);remaining=[x for x in members(proc.pid) if 'Z' not in x['state']];after=probe();final,end,live,rows,diag_errors=extract_diagnostics(s)
    result={'exit_code':code,'requested_seconds':seconds,'minimum_seconds':seconds,'wall_seconds':time.monotonic()-started,'before_state':before,'after_state':after,'remaining_processes':remaining,'device_lock_released':lock_free(),'final_status':final,'end':end,'forced_stop':forced,'live_add_event_count':len(live)}
    write(s/'result.json',result);event('receiver_exited',stage=name,exit_code=code,final_state=after.get('firmware_state'))
    failures=[]
    if forced:failures.append('receiver forced stop: '+forced)
    if code!=0:failures.append('receiver exit code: '+str(code))
    if remaining:failures.append('receiver process group still has members')
    if group_cleanup['had_survivors']:failures.append('receiver root exited before its owned child processes; cleanup was required')
    if after.get('firmware_state')!=16:failures.append('device state is not idle16')
    if after.get('state_notifications_pending') is not False:failures.append('device state notifications remain pending')
    if not result['device_lock_released']:failures.append('device lock remains held')
    if not final or not end:failures.append('missing DT295 confirmed-byte status or final end event')
    if len([x for x in rows if x.get('event')=='end'])!=1:failures.append('final end event count is not exactly one')
    if diag_errors:failures.append('diagnostic extraction had malformed or oversized input')
    if capture.exists() and final:
        size=capture.stat().st_size
        counters={'capture_size_bytes':size,'capture_packets':size//188,'confirmed_bytes':final.get('confirmed_bytes'),'end_packets':(end or {}).get('packets'),'end_state':(end or {}).get('state'),'usb_delivered_bytes':final.get('usb_delivered_bytes'),'trailing_bytes':(end or {}).get('trailing_bytes'),'stop_drained_bytes':(end or {}).get('stop_drained_bytes'),'pending_bytes':final.get('pending_bytes'),'rejected_bytes':final.get('rejected_bytes'),'uncertain_bytes':final.get('uncertain_bytes'),'usb_max_gap_ns':final.get('usb_max_gap_ns')}
        trailing=counters['trailing_bytes'];drained=counters['stop_drained_bytes'];gap=counters['usb_max_gap_ns']
        delivered=size+(trailing or 0)+(drained or 0)
        consistent=(size%188==0 and counters['confirmed_bytes']==size and counters['end_packets']==size//188 and counters['end_state']==16 and type(trailing) is int and trailing>=0 and type(drained) is int and drained>=0 and counters['usb_delivered_bytes']==delivered and type(gap) is int and gap<1_000_000_000 and all(final.get(k)==0 for k in ('pending_bytes','rejected_bytes','uncertain_bytes','usb_error','usb_timeouts','writer_error')))
        write(s/'diagnostic-counter-check.json',{'consistent':consistent,**counters})
        if not consistent:failures.append('final diagnostic counters disagree with closed capture')
    # Always audit a saved TS before applying receiver exit/live-event gates, including exit 8.
    audit_code=None;script=P/'audit.py'
    if capture.exists() and capture.stat().st_size:
        state('auditing',stage=name);audit_cmd=[sys.executable,str(script),str(s)];audit=None;started_audit=time.time();deadline=cfg['audit_timeout_seconds'];audit_interrupted=False
        with (s/'audit-console.log').open('xb') as log:
            audit=subprocess.Popen(audit_cmd,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            write(s/'audit-launch.json',{'pid':audit.pid,'command':audit_cmd,'process_command':audit_command(audit.pid),'started_unix':started_audit,'deadline_seconds':deadline})
            try:audit_code=audit.wait(timeout=deadline)
            except BaseException as exc:
                audit_interrupted=True;audit_code=audit.poll()
                try:terminate_audit(audit,s,script,'audit interrupted: '+repr(exc))
                except BaseException as ce:cleanup_event('audit_exception_cleanup_failed',pid=audit.pid,original_error=repr(exc),cleanup_error=repr(ce))
                write(s/'audit-exit.json',{'pid':audit.pid,'ended_unix':time.time(),'exit_code':audit.poll(),'interrupted':repr(exc),'cleanup_complete':not members(audit.pid)})
                if isinstance(exc,subprocess.TimeoutExpired):failures.append('whole TS audit timed out')
                else:raise
        if not audit_interrupted:write(s/'audit-exit.json',{'pid':audit.pid,'ended_unix':time.time(),'exit_code':audit_code,'interrupted':False})
    else:
        write(s/'audit-not-run.json',{'reason':'saved capture is missing or empty; no whole-file audit possible'})
        failures.append('saved TS missing; audit could not run')
    review_script=P/'live-add-evidence-review.py';review_code=None;review=None;review_result=None
    if capture.exists() and capture.stat().st_size and review_script.exists():
        review_cmd=[sys.executable,str(review_script),str(s)];review=None;review_started=time.time();review_deadline=cfg['live_review_timeout_seconds'];review_interrupted=False
        with (s/'live-add-review-console.log').open('xb') as log:
            review=subprocess.Popen(review_cmd,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            write(s/'live-add-review-launch.json',{'pid':review.pid,'command':review_cmd,'process_command':audit_command(review.pid),'started_unix':review_started,'deadline_seconds':review_deadline})
            try:review_code=review.wait(timeout=review_deadline)
            except BaseException as exc:
                review_interrupted=True;review_code=review.poll()
                try:terminate_audit(review,s,review_script,'bounded live-add review interrupted: '+repr(exc))
                except BaseException as ce:cleanup_event('live_review_cleanup_failed',pid=review.pid,original_error=repr(exc),cleanup_error=repr(ce))
                write(s/'live-add-review-exit.json',{'pid':review.pid,'ended_unix':time.time(),'exit_code':review.poll(),'interrupted':repr(exc),'cleanup_complete':not members(review.pid)})
                if isinstance(exc,subprocess.TimeoutExpired):failures.append('bounded live-add review timed out')
                else:raise
        if not review_interrupted:write(s/'live-add-review-exit.json',{'pid':review.pid,'ended_unix':time.time(),'exit_code':review_code,'interrupted':False})
        review_json=s/'live-add-evidence-review.json'
        if review_json.exists():
            try:review_result=json.loads(review_json.read_text())
            except (OSError,json.JSONDecodeError):review_result=None
        if review_code!=0 or not isinstance(review_result,dict):
            failures.append('bounded live-add evidence review failed or produced no JSON result')
        else:
            required=('analysis_valid','fix_path_verdict','live_add_count','minimum_post_add_seconds_observed')
            if any(k not in review_result for k in required) or review_result.get('analysis_valid') is not True:
                failures.append('bounded live-add evidence review result is incomplete or invalid')
            elif item.get('require_live_add') and (review_result.get('fix_path_verdict')!='observed_pass' or type(review_result.get('minimum_post_add_seconds_observed')) not in (int,float) or review_result.get('minimum_post_add_seconds_observed')<1800):
                failures.append('five-hour live-add path lacks observed pass with 30 minutes of post-add evidence')
            elif item.get('require_live_add') and (type(review_result.get('live_add_count')) is not int or review_result.get('live_add_count')<1):
                failures.append('five-hour live-add event count is zero or unavailable')
    else:
        write(s/'live-add-review-not-run.json',{'reason':'capture missing/empty or bounded review helper absent','helper_present':review_script.exists()})
        failures.append('bounded live-add evidence review did not run')
    # Startup routing is not a hardware readback. Preserve the available event/guard evidence and its limits.
    write(s/'live-add-evidence-limitations.json',{'committed_events':live,'summary_events':[x for x in rows if x.get('event')=='dt295_live_add_summary'],'limitations':['The DT295 status stream does not expose a startup hardware route-register dump.','A live-add event is emitted only after all route writes succeed and software route state commits; its preservation flag reflects the candidate guard.','Initial PMT mappings must be established from saved transport PSI by the separately bounded live-add evidence reviewer.','No device register readback is claimed.']})
    if audit_code not in (0,):failures.append('whole TS audit did not pass')
    if item.get('require_live_add') and not live:failures.append('five-hour live-add event not observed; fix-path validation is inconclusive')
    write(s/'stage-gate-results.json',{'passed':not failures,'failures':failures,'audit_exit_code':audit_code,'live_add_event_count':len(live),'live_add_review_exit_code':review_code,'live_add_review_result':review_result})
    if failures:raise RuntimeError('stage quality gate failed after preserving audit evidence: '+name+'; '+'; '.join(failures))
    event('stage_passed',stage=name,live_add_event_count=len(live));return name
def main():
    if sys.argv[1:]!=['--execute']:raise SystemExit('use start_once.py; direct execution is disabled')
    marker=json.loads(MARK.read_text())
    if marker.get('pid')!=os.getpid() or marker.get('consumed') is not True:raise RuntimeError('one-shot launch marker mismatch')
    cfg=json.loads(CFG.read_text());pre=preflight();verify_helpers();verify_harness_manifest(pre)
    if Path(cfg['external_directory'])!=DATA:raise RuntimeError('configured external path mismatch')
    if hashlib.sha256((P/'assets/dt295.assets').read_bytes()).hexdigest()!=pre.get('assets_sha256'):raise RuntimeError('assets fingerprint mismatch')
    if hashlib.sha256(BIN.read_bytes()).hexdigest()!=cfg['binary_sha256']:raise RuntimeError('development binary SHA mismatch')
    if fingerprint()!=cfg['source_fingerprint_sha256']:raise RuntimeError('frozen source fingerprint mismatch')
    if json.loads((P/'source-hashes.json').read_text()).get('patch_sha256')!=cfg['candidate_patch_sha256']:raise RuntimeError('r8 candidate patch SHA mismatch')
    os.umask(0o077);controller=open(P/'campaign-controller.lock','a');fcntl.flock(controller,fcntl.LOCK_EX|fcntl.LOCK_NB)
    prior=P.parent/'pre-rc-01';old_controller=open(prior/'controller.lock','a');fcntl.flock(old_controller,fcntl.LOCK_EX|fcntl.LOCK_NB)
    old_long=open(prior/'long-controller.lock','a');fcntl.flock(old_long,fcntl.LOCK_EX|fcntl.LOCK_NB)
    legacy=open(ROOT/'work/pix-dt295-static/device.lock','a');fcntl.flock(legacy,fcntl.LOCK_EX|fcntl.LOCK_NB)
    DATA.mkdir(mode=0o700,parents=True,exist_ok=True)
    if not (P/'stages').exists():(P/'stages').mkdir(mode=0o700)
    if any((P/'stages').iterdir()):raise RuntimeError('stages directory is not empty; campaign is one shot')
    done=[]
    try:
        # Recheck fresh before campaign and every stage does another idle16/lock probe.
        now=probe()
        if now.get('firmware_state')!=16 or now.get('state_notifications_pending') is not False or not lock_free():raise RuntimeError('launch-time device state is not idle16/notification-free/unlocked')
        for item in cfg['stages']:done.append(one(item,cfg))
        state('complete_pending_review',completed_stages=done);event('campaign_complete_pending_review',stages=done)
    except BaseException as exc:
        state('needs_attention',completed_stages=done,error=repr(exc));cleanup_event('needs_attention',error=repr(exc),completed_stages=done);raise
if __name__=='__main__':main()
