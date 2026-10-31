#!/usr/bin/env python3
"""A .chimeraProject for chimera-run, the way a person's project names a game.

    make-project.py <package> <out.chimeraProject> <script> <frames>
                    [--set=<setting>=<value>]... [--firmware=<id>=<file>]...
                    <slot>=<file>...

A project mounts each file in its slot under its own name, which the bare
file chimera-run is otherwise given never does. The input log is the
harness script (gate-harness.h) as rows of this package's controls, with the
LogKey the engine reads them by. The firmware is pinned by the hash of the
file given for it, as the wizard pins it; the files themselves come from
chimera-run's --firmware.
"""
import hashlib
import importlib.util
import json
import os
import sys
import zipfile


def inputs_of(log):
    """A movie's text block ("[Input]", "LogKey:...", its entries, "[/Input]")
    as a project file's "inputs": the key's groups of column names, and one
    entry a frame. A group starts where a '#' begins a name; a name ends at
    '|' (as Chimera splits a key)."""
    lines = log.splitlines()
    out = {}
    key = next((l[len("LogKey:"):] for l in lines if l.startswith("LogKey:")), None)
    if key is not None:
        groups, at = [], 0
        while at < len(key):
            assert key[at] == "#", key
            groups.append([])
            at += 1
            while at < len(key) and key[at] != "#":
                bar = key.index("|", at)
                groups[-1].append(key[at:bar])
                at = bar + 1
        out["columns"] = groups
    out["entries"] = [l for l in lines if l.startswith("|")]
    return out

# the rows are make-movie.py's (its name has a dash: loaded by path)
spec = importlib.util.spec_from_file_location(
    "make_movie", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make-movie.py"))
make_movie = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_movie)


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    package, out, script, frames = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    cfg = json.loads(zipfile.ZipFile(package).read("waterbox.config"))

    chosen, firmware_files, files = {}, {}, []
    for arg in sys.argv[5:]:
        if arg.startswith("--set="):
            name, value = arg[len("--set="):].split("=", 1)
            chosen[name] = value
        elif arg.startswith("--firmware="):
            fid, path = arg[len("--firmware="):].split("=", 1)
            firmware_files[fid] = path
        else:
            slot, path = arg.split("=", 1)
            files.append({"name": os.path.basename(path), "sha1": sha1(path), "slot": slot})

    settings = {d["name"]: d["default"] for d in cfg["settings"] if "default" in d}
    for name, value in chosen.items():
        if name not in settings:
            sys.exit("make-project.py: the package declares no setting named " + name)
        settings[name] = int(value) if isinstance(settings[name], int) else value

    # the engine's groups: the console (Touch X, Touch Y, Touch), then each
    # player, axes before buttons - make-movie.py writes its rows so
    inputs = cfg["input"]
    def group(prefix):
        names = [a["name"] for a in inputs["axes"] if a["name"].startswith(prefix)]
        names += [b for b in inputs["buttons"] if b.startswith(prefix)]
        return "#" + "".join(n + "|" for n in names)
    console = "#" + "".join(n + "|" for n in ("Touch X", "Touch Y", "Touch"))
    key = console + group("P1 ") + group("P2 ")
    lines = make_movie.read_script(script)
    rows = [make_movie.row(*make_movie.at(lines, f)) for f in range(1, frames + 1)]
    log = "[Input]\nLogKey:" + key + "\n" + "".join(r + "\n" for r in rows) + "[/Input]\n"

    firmware = []
    for d in cfg.get("firmware", []):
        if d["id"] in firmware_files:
            firmware.append({"id": d["id"], "sha1": sha1(firmware_files[d["id"]])})
        elif d.get("required"):
            sys.exit("make-project.py: %s is required; say which file with --firmware=%s=<file>" % (d["id"], d["id"]))

    game = next((f for f in files if f["slot"] == cfg.get("romFile", "game")), files[0] if files else None)
    project = {
        "id": "eden-gate-01",
        "title": "Switch through Chimera",
        "description": "written by waterbox/tests/make-project.py",
        "core": {"name": cfg["coreName"], "version": "", "sha1": ""},
        "rerecords": 0,
        "files": files,
        "settings": settings,
        "firmware": firmware,
        "coreCache": [],
        "inputs": inputs_of(log),
        "markers": [],
        "branches": [],
        "headers": {
            "MovieVersion": "Chimera Project File v1.1",
            "Platform": cfg["systemId"],
            "SHA1": game["sha1"] if game else "",
            "LastInputFrame": str(frames - 1),
            "VsyncNumerator": str(cfg["video"]["vsyncNumerator"]),
            "VsyncDenominator": str(cfg["video"]["vsyncDenominator"]),
        },
    }
    with open(out, "w") as f:
        json.dump(project, f, indent="\t")
    return 0


if __name__ == "__main__":
    sys.exit(main())
