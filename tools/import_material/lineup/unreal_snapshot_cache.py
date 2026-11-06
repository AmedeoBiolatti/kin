"""Unreal's snapshots, cached: a material is pictured again only when what makes its picture changed.

    unreal_snapshot_cache.py --list <materials.txt> --cache <dir> --project <Project.uproject> --ue <ue.sh>
                             [--scene snapshot.json] [--assets out/lineup/assets] [--seed <dir of pictures>]

Each picture's key: the scene file, unreal_snapshots.py (the harness: lights, settings, meshes) and the material's
.uasset (its size and time; a parent's change is not seen: --force). The cache holds <name>.png, <name>.key and
<name>.time (the world's clock as it was pictured: kin_material_lineup --time); times.txt lists every material of
the list. The editor runs only for the misses (unreal_snapshots.py with SNAP_LIST of them). --seed takes pictures
made by the current harness (and their times.txt) into the cache without rendering.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]


def picture_name(kin_path):
    return kin_path.replace("/", "__") + ".png"


def uasset(project, kin_path):
    rest = kin_path[2:] if kin_path.startswith("m/") else kin_path
    return os.path.join(os.path.dirname(project), "Content", rest + ".uasset")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--ue", required=True)
    ap.add_argument("--scene", default=os.path.join(HERE, "snapshot.json"))
    ap.add_argument("--assets", default="out/lineup/assets")
    ap.add_argument("--seed")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    harness = digest(a.scene) + "-" + digest(os.path.join(HERE, "unreal_snapshots.py"))
    materials = [l.split()[0] for l in open(a.list) if l.strip() and not l.startswith("#")]

    def key(m):
        p = uasset(a.project, m)
        st = os.stat(p) if os.path.exists(p) else None
        return harness + "-" + (f"{st.st_size}-{int(st.st_mtime)}" if st else "missing")

    def cached(m):
        n = picture_name(m)
        k = os.path.join(a.cache, n[:-4] + ".key")
        return (not a.force and os.path.exists(os.path.join(a.cache, n)) and os.path.exists(k)
                and open(k).read().strip() == key(m))

    def store(m, picture, time):
        n = picture_name(m)
        if os.path.abspath(picture) != os.path.abspath(os.path.join(a.cache, n)):
            shutil.copyfile(picture, os.path.join(a.cache, n))
        open(os.path.join(a.cache, n[:-4] + ".key"), "w").write(key(m) + "\n")
        open(os.path.join(a.cache, n[:-4] + ".time"), "w").write(time + "\n")

    if a.seed:
        times = {}
        for line in open(os.path.join(a.seed, "times.txt")):
            m, t = line.split()[:2]
            times[m] = t
        for m in materials:
            if m in times and os.path.exists(os.path.join(a.seed, picture_name(m))):
                store(m, os.path.join(a.seed, picture_name(m)), times[m])
    misses = [m for m in materials if not cached(m)]
    print(f"unreal snapshots: {len(materials) - len(misses)} cached, {len(misses)} to picture")
    if misses:
        todo = os.path.join(a.cache, "_todo.txt")
        open(todo, "w").write("".join(m + "\n" for m in misses))
        fresh = os.path.join(a.cache, "_fresh")
        shutil.rmtree(fresh, ignore_errors=True)
        os.makedirs(fresh)
        env = dict(os.environ, LINEUP_SCRIPT=os.path.join(HERE, "unreal_snapshots.py"),
                   SNAP_SCENE=os.path.abspath(a.scene), SNAP_ASSETS=os.path.abspath(a.assets),
                   SNAP_LIST=os.path.abspath(todo), SNAP_OUT=os.path.abspath(fresh))
        subprocess.run([a.ue, "--mem", "12G", "editor", a.project, "-nosplash", "-unattended"], env=env,
                       stdout=open(os.path.join(a.cache, "_editor.log"), "w"), stderr=subprocess.STDOUT)
        log = os.path.join(os.path.dirname(a.project), "Saved", "Logs",
                           os.path.splitext(os.path.basename(a.project))[0] + ".log")
        times = {}
        for line in open(log, errors="replace"):
            hit = re.search(r"lineup: time (\S+) (\S+)", line)
            if hit:
                times[hit.group(1)] = hit.group(2)  # (the last a material)
        for m in misses:
            p = os.path.join(fresh, picture_name(m))
            if os.path.exists(p) and m in times:
                store(m, p, times[m])
            else:
                print("unreal snapshots: no picture of " + m, file=sys.stderr)
        shutil.rmtree(fresh, ignore_errors=True)
    with open(os.path.join(a.cache, "times.txt"), "w") as out:
        for m in materials:
            t = os.path.join(a.cache, picture_name(m)[:-4] + ".time")
            if os.path.exists(t):
                out.write(m + " " + open(t).read().strip() + "\n")


if __name__ == "__main__":
    main()
