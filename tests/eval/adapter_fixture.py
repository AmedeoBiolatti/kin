"""A fourth scene using only the public command-job protocol."""

import json
from pathlib import Path
import sys
import struct
import zlib

job = json.loads(Path(sys.argv[1]).read_text())
out = Path(sys.argv[2])
profile = job["profile"]
size = profile["size"]
kind = job["kind"]


def png(path):
    w, h = size

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + bytes((32, 64, 128)) * w for _ in range(h))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


result = {"schema": "kin.eval-result/1", "status": "ok", "size": size, "sequences": []}
if kind in ("bench", "stress"):
    with (out / "frames.jsonl").open("w") as f:
        for i in range(profile["frames"]):
            f.write(
                json.dumps(
                    {
                        "schema": "kin.eval-frame/1",
                        "view": "fixture",
                        "frame": i,
                        "cpu": {"interval_ms": 1 + i * 0.01, "work_ms": 0.8},
                        "gpu": {
                            "method": "fence_completion_estimate",
                            "sample_serial": 1,
                            "latest_ms": 0.5,
                            "percentiles_valid": False,
                        },
                        "memory": {"owned_bytes": 1024, "allocations": 1},
                    }
                )
                + "\n"
            )
    result["frames_file"] = "frames.jsonl"
else:
    frames = []
    for i in range(3):
        name = f"{i}.png"
        png(out / name)
        frames.append({"frame": i, "image": name})
    result["sequences"] = [
        {
            "id": "fixture",
            "mode": "static",
            "fps": 60,
            "transform": {"kind": "srgb"},
            "frames": frames,
        }
    ]
if kind == "truth":
    result["seeds"] = {str(seed): result["sequences"] for seed in profile["seeds"]}
    result["sequences"] = []
    result["reference_role"] = "fixture"
(out / "result.json").write_text(json.dumps(result))
