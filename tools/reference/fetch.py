"""The reference scenes' files (docs/3d_plan.md, G0) into the cache: those
made here (make_scenes.py) and those downloaded, pinned to a commit of
KhronosGroup/glTF-Sample-Assets and checked against assets.lock.json.

    python3 tools/reference/fetch.py            # all, into the cache
    python3 tools/reference/fetch.py --lock     # (maintainers) write the lock from what is downloaded

The cache: $KIN_REFERENCE_CACHE, else ~/.cache/kin/reference. Nothing
downloaded is committed: Sponza's licence (Crytek's) allows using it, not
passing it on.
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = "KhronosGroup/glTF-Sample-Assets"
COMMIT = "edc7c9e67c639d230715049ee31f9a96a6babbbe"
ASSETS = {
    # name: (folder in the repository, licence)
    "flight_helmet": ("Models/FlightHelmet/glTF", "CC0 1.0 (Gary Hsu; Khronos glTF Sample Assets)"),
    "sponza": ("Models/Sponza/glTF", "Cryengine Limited License Agreement (Crytek; Khronos glTF Sample Assets): not redistributed"),
}
LOCK = os.path.join(HERE, "assets.lock.json")


def cache_dir():
    return os.environ.get("KIN_REFERENCE_CACHE") or os.path.join(os.path.expanduser("~"), ".cache", "kin", "reference")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def listing(folder):
    url = f"https://api.github.com/repos/{REPO}/contents/{folder}?ref={COMMIT}"
    with urllib.request.urlopen(url, timeout=60) as r:
        entries = json.load(r)
    return sorted(e["name"] for e in entries if e["type"] == "file")


def download(folder, name, to):
    url = f"https://raw.githubusercontent.com/{REPO}/{COMMIT}/{folder}/{name}"
    tmp = to + ".part"
    with urllib.request.urlopen(url, timeout=300) as r, open(tmp, "wb") as f:
        while block := r.read(1 << 20):
            f.write(block)
    os.replace(tmp, to)


def main():
    write_lock = "--lock" in sys.argv
    root = cache_dir()
    subprocess.run([sys.executable, os.path.join(HERE, "make_scenes.py"), os.path.join(root, "made")], check=True)
    lock = {} if write_lock else json.load(open(LOCK))["assets"]
    for name, (folder, licence) in ASSETS.items():
        out = os.path.join(root, name)
        os.makedirs(out, exist_ok=True)
        files = listing(folder) if write_lock else sorted(lock[name]["files"])
        sums = {}
        for f in files:
            path = os.path.join(out, f)
            want = None if write_lock else lock[name]["files"][f]
            if not (os.path.isfile(path) and (want is None or sha256(path) == want)):
                print(f"{name}: {f}", flush=True)
                download(folder, f, path)
            got = sha256(path)
            if want is not None and got != want:
                sys.exit(f"{name}/{f}: SHA-256 {got}, the lock says {want}")
            sums[f] = got
        if write_lock:
            lock[name] = {"folder": folder, "licence": licence, "files": sums}
        print(f"{name}: {len(files)} files in {out}")
    if write_lock:
        with open(LOCK, "w") as f:
            json.dump({"repository": REPO, "commit": COMMIT, "assets": lock}, f, indent=1, sort_keys=True)
            f.write("\n")


if __name__ == "__main__":
    main()
