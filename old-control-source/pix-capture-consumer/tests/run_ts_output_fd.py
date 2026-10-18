import argparse
import os
import signal
import subprocess
import sys
import time


class WrapperInterrupted(BaseException):
    def __init__(self, signum):
        self.signum = signum


def interrupt_wrapper(signum, _frame):
    raise WrapperInterrupted(signum)


def group_has_live_processes(group):
    proc = "/proc"
    for name in os.listdir(proc):
        if not name.isdigit():
            continue
        try:
            with open(os.path.join(proc, name, "stat"), encoding="ascii") as stream:
                fields = stream.read().rsplit(")", 1)[1].split()
            if int(fields[2]) == group and fields[0] != "Z":
                return True
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, IndexError):
            continue
    return False


def stop_group(group):
    if not group_has_live_processes(group):
        return True
    try:
        os.killpg(group, signal.SIGTERM)
    except ProcessLookupError:
        return True
    deadline = time.monotonic() + 0.5
    while time.monotonic() < deadline and group_has_live_processes(group):
        time.sleep(0.02)
    if group_has_live_processes(group):
        try:
            os.killpg(group, signal.SIGKILL)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and group_has_live_processes(group):
        time.sleep(0.02)
    return not group_has_live_processes(group)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not command:
        parser.error("a test command is required")
    child = subprocess.Popen(command, start_new_session=True)
    group = child.pid
    try:
        signal.signal(signal.SIGINT, interrupt_wrapper)
        signal.signal(signal.SIGTERM, interrupt_wrapper)
        try:
            return_code = child.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            cleaned = stop_group(group)
            child.wait()
            if not cleaned:
                print("FD test timed out; its process group still has live descendants", file=sys.stderr)
            else:
                print("FD test timed out and its process group was stopped", file=sys.stderr)
            return 124

        if group_has_live_processes(group):
            cleaned = stop_group(group)
            if not cleaned:
                print("FD test exited with live descendants that could not be stopped", file=sys.stderr)
            if return_code == 0:
                print("FD test exited successfully but left live descendants", file=sys.stderr)
                return 125
            if return_code != 0:
                print("FD test exited with status %d; its process group was stopped" % return_code,
                      file=sys.stderr)
        return return_code
    except WrapperInterrupted as interrupted:
        stop_group(group)
        if child.poll() is None:
            child.wait()
        return 128 + interrupted.signum
    except KeyboardInterrupt:
        stop_group(group)
        if child.poll() is None:
            child.wait()
        return 128 + signal.SIGINT
    except BaseException:
        stop_group(group)
        if child.poll() is None:
            child.wait()
        raise


if __name__ == "__main__":
    sys.exit(main())
