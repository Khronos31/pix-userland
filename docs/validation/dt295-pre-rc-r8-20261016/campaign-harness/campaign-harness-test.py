"""Offline campaign harness regression checks. Never invokes USB or controller."""
import hashlib,importlib.util,json,os,py_compile,subprocess,sys,tempfile,time
from pathlib import Path
P=Path(__file__).resolve().parent;CTRL=P/'campaign-controller.py'
for src in (CTRL,P/'start_once.py'):py_compile.compile(str(src),doraise=True)
spec=importlib.util.spec_from_file_location('campaign_controller_test',CTRL);ctl=importlib.util.module_from_spec(spec);spec.loader.exec_module(ctl)
checks=[]
# Event summary with phase=3 cannot be selected as final status.
with tempfile.TemporaryDirectory(prefix='r8-status-filter-') as td:
    stage=Path(td);diag=stage/'diagnostic';diag.mkdir()
    rows=[{'model':'DT295','phase':2,'confirmed_bytes':99,'packets':1},{'event':'dt295_live_add_summary','phase':3,'live_add_count':1},{'event':'end','state':16}]
    (diag/'status.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in rows))
    final,end,live,allrows,errors=ctl.extract_diagnostics(stage)
    assert final==rows[0] and end==rows[2] and live==[] and not errors and (stage/'all-events.jsonl').read_text().count('\n')==2
    assert json.loads((stage/'status-samples.jsonl').read_text())==rows[0]
    checks.append('phase-only live-add summary is retained as an event and cannot replace DT295 confirmed-byte status')
# Process cleanup regression, using only synthetic sleep/audit subprocesses.
with tempfile.TemporaryDirectory(prefix='r8-process-cleanup-') as td:
    d=Path(td);ctl.P=d;cap=d/'capture.ts'
    child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)',str(ctl.BIN),str(cap)],start_new_session=True)
    time.sleep(.12);assert ctl.owned(child.pid,cap);ctl.terminate(child,cap,'synthetic')
    assert child.poll() is not None and not ctl.members(child.pid)
    checks.append('validated synthetic receiver command is stopped and reaped')
    cap2=d/'child-capture.ts'
    child_root=subprocess.Popen([sys.executable,'-c','import subprocess,sys,time;subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)",sys.argv[1],sys.argv[2]]);time.sleep(.2)',str(ctl.BIN),str(cap2)],start_new_session=True)
    child_root.wait(timeout=5);time.sleep(.1)
    cleanup=ctl.close_receiver_group(child_root.pid,cap2)
    assert cleanup['safe_to_inspect'] and cleanup['had_survivors'] and not cleanup['remaining'],cleanup
    checks.append('synthetic child left after root exit is command-validated, stopped, and gone before inspection')
    cap3=d/'unknown-capture.ts'
    unknown_root=subprocess.Popen([sys.executable,'-c','import subprocess,sys,time;subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"]);time.sleep(.2)',str(ctl.BIN),str(cap3)],start_new_session=True)
    unknown_root.wait(timeout=5);time.sleep(.1);unknown_pid=next(x['pid'] for x in ctl.members(unknown_root.pid) if 'Z' not in x['state'])
    refused=ctl.close_receiver_group(unknown_root.pid,cap3)
    assert not refused['safe_to_inspect'] and refused.get('unverified')
    os.kill(unknown_pid,15)
    checks.append('unverified synthetic group member is not signaled and capture inspection is blocked')
    bindir=d/'bin';bindir.mkdir();os.symlink('/bin/sleep',bindir/'ffmpeg');audscript=d/'audit.py';audscript.write_text('import subprocess,time\nsubprocess.Popen(["ffmpeg","30"])\ntime.sleep(30)\n')
    env=os.environ.copy();env['PATH']=str(bindir)+os.pathsep+env.get('PATH','')
    aud=subprocess.Popen([sys.executable,str(audscript),str(d)],env=env,start_new_session=True);time.sleep(.25)
    assert any(Path(x['command'].split()[0]).name=='ffmpeg' for x in ctl.members(aud.pid))
    ctl.terminate_audit(aud,d,audscript,'synthetic timeout');assert aud.poll() is not None and not ctl.members(aud.pid)
    checks.append('audit timeout cleanup terminates synthetic audit parent and validated ffmpeg child')
ctl.P=P
# Fingerprint and ordering checks are static/offline; start_once and controller are never executed.
config=json.loads((P/'campaign-config.json').read_text());assert ctl.fingerprint()==config['source_fingerprint_sha256']
assert hashlib.sha256(ctl.BIN.read_bytes()).hexdigest()==config['binary_sha256']
pre=(P/'start_once.py').read_text();assert all(x in pre for x in ('software_cross_validation_passed','hardware_launch_authorized','source_binary_harness_verified'))
assert "if __name__=='__main__':main()" in pre
source=CTRL.read_text();assert "if any((P/'stages').iterdir())" in source
assert source.index("audit_code=audit.wait(timeout=deadline)") < source.index("if item.get('require_live_add') and not live")
assert "if capture.exists() and capture.stat().st_size:" in source and "audit-not-run.json" in source
assert "audit-launch.json" in source and "audit-exit.json" in source
assert 'close_receiver_group(proc.pid,capture)' in source and "unverified or surviving process may still mutate capture.ts" in source
assert "if not audit_interrupted:write(s/'audit-exit.json'" in source
assert "if not review_interrupted:write(s/'live-add-review-exit.json'" in source
assert "review_cmd=[sys.executable,str(review_script),str(s)]" in source
assert "review_result.get('fix_path_verdict')!='observed_pass'" in source and "minimum_post_add_seconds_observed')<1800" in source
assert source.index("if code!=0:failures.append") < source.index("audit_code=audit.wait(timeout=deadline)")
ctl.verify_harness_manifest(json.loads((P/'preflight.json').read_text()))
checks.append('frozen fingerprints, preflight gates, nonempty-stage guard, and audit-before-live-add gate are wired')
result={'passed':True,'controller_launched':False,'USB_access':False,'receiver_started':False,'checks':checks}
(P/'campaign-harness-test.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
