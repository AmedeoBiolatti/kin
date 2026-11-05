"""Child-process helpers shared by the automation tools and kin_dev.py."""

import os
import signal
import subprocess


def _signal_group(process, sig):
    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        pass


def stop_process_group(process, grace=2):
    """Ask a child started with start_new_session=True (and its children) to stop,
    then kill it after `grace` seconds. Windows has no process groups: the child
    alone is terminated, then killed."""
    if process is None:
        return
    if os.name != "nt":
        _signal_group(process, signal.SIGTERM)
    elif process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            _signal_group(process, signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=grace)


def end_process_group(process):
    """After the child exited by itself, stop what it left running in its group."""
    if os.name != "nt":
        _signal_group(process, signal.SIGTERM)
