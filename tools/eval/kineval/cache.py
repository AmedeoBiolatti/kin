"""Reference identity excludes engine builds; scene/camera/provider changes invalidate it."""

from .common import fingerprint


def truth_identity(job, observed):
    scene = job["scene"]
    capture = job["capture"]
    provider = scene.get("reference", {}).get("provider", scene["adapter"])
    cameras = []
    for seq in capture["sequences"]:
        cameras.append(
            {k: v for k, v in seq.items() if k not in ("frames", "transform")}
        )
        cameras[-1]["frames"] = [
            {k: v for k, v in f.items() if k != "image"} for f in seq["frames"]
        ]
    content = dict(observed["content"][scene["id"]])
    manifest = scene.get("manifest")
    if manifest != scene.get("source"):
        content.pop(manifest, None)
    mapping = {
        k: scene[k]
        for k in ("id", "source", "asset_root", "script", "reference", "saturation")
        if k in scene
    }
    if provider == "command":
        mapping = {k: v for k, v in scene.items() if k not in ("manifest", "_base")}
    return fingerprint(
        {
            "schema": "kin.eval-truth-cache/1",
            "mapping": mapping,
            "content": content,
            "cameras": cameras,
            "unmapped_truth_flags": capture.get("unmapped_truth_flags", []),
            "size": job["profile"]["size"],
            "samples": job["profile"]["samples"],
            "seeds": job["profile"]["seeds"],
            "blender": observed.get("tool_files", {}).get("blender"),
            "adapter": observed["code"].get("tools/eval/kineval/adapters.py"),
            "reference_script": observed["code"].get(
                "tools/reference/blender_truth.py"
            ),
            "render_context": observed.get("render_context"),
        }
    )
