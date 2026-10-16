"""Creates the sole launch authorization marker and execs the staged controller."""
import fcntl,hashlib,importlib.util,json,os,shutil,subprocess,sys,time
from pathlib import Path
P=Path(__file__).resolve().parent
ROOT=P.parents[2]

def lock_free(path,kind='flock'):
    with Path(path).open('a+') as f:
        try:
            if kind=='posix':fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB);fcntl.lockf(f,fcntl.LOCK_UN)
            else:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB);fcntl.flock(f,fcntl.LOCK_UN)
            return True
        except BlockingIOError:return False

def main():
    if sys.argv[1:]:raise SystemExit('start_once.py accepts no arguments')
    pre=json.loads((P/'preflight.json').read_text())
    for key in ('software_cross_validation_passed','hardware_launch_authorized','source_binary_harness_verified'):
        if pre.get(key) is not True:raise SystemExit('preflight gate required: '+key)
    binary=P/'development-r8/pix-ts';digest=hashlib.sha256(binary.read_bytes()).hexdigest()
    cfg=json.loads((P/'campaign-config.json').read_text())
    if digest!=cfg['binary_sha256'] or pre.get('binary_sha256')!=digest:raise SystemExit('binary fingerprint changed since preflight')
    if pre.get('assets_sha256')!=hashlib.sha256((P/'assets/dt295.assets').read_bytes()).hexdigest():raise SystemExit('assets fingerprint changed since preflight')
    spec=importlib.util.spec_from_file_location('campaign_controller_precheck',P/'campaign-controller.py')
    controller=importlib.util.module_from_spec(spec);spec.loader.exec_module(controller)
    if controller.fingerprint()!=cfg['source_fingerprint_sha256']:raise SystemExit('frozen source fingerprint mismatch')
    controller.verify_helpers()
    controller.verify_harness_manifest(pre)
    if shutil.disk_usage(P).free<cfg['internal_minimum_free_bytes']:raise SystemExit('internal storage floor unavailable')
    external=Path(cfg['external_directory'])
    if not external.exists() or shutil.disk_usage(external).free<cfg['external_runtime_minimum_free_bytes']:raise SystemExit('external storage floor unavailable')
    old=P.parent/'pre-rc-01'
    checks=[(old/'controller.lock','flock'),(old/'long-controller.lock','flock'),
            (P/'controller.lock','flock'),(P/'long-controller.lock','flock'),(P/'campaign-controller.lock','flock'),
            (ROOT/'work/pix-dt295-static/device.lock','flock'),(Path('/tmp/pix-userland-dt295-'+str(os.getuid())+'.lock'),'posix')]
    if not all(lock_free(path,kind) for path,kind in checks):raise SystemExit('existing controller/device lock is held')
    state=json.loads(subprocess.check_output([sys.executable,str(P/'read_state.py')],text=True,timeout=10))
    if state.get('firmware_state')!=16 or state.get('state_notifications_pending') is not False:raise SystemExit('device is not idle state 16 with no pending notifications; stop for physical review')
    marker=P/'start-once.json'
    fd=os.open(marker,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w') as f:json.dump({'pid':os.getpid(),'created_unix':time.time(),'binary_sha256':digest,'consumed':True},f);f.write('\n');f.flush();os.fsync(f.fileno())
    os.execv(sys.executable,[sys.executable,str(P/'campaign-controller.py'),'--execute'])
if __name__=='__main__':main()
