#!/usr/bin/env python3
"""Writes the next patch of the series: whatever the extern/eden working
tree changes beyond what the series already leaves behind.

The trap: `git diff` on a file two patches touch produces a
patch containing both. So the baseline here is not HEAD but HEAD with every
existing patch applied (worked out on a scratch copy, as apply-patches.sh
does), and only the difference from THAT goes into the new patch.

usage: make-patch.py <name>         e.g. 0002-chimera-openal-without-host-audio
       make-patch.py --check        say what a new patch would hold, write nothing
       make-patch.py --amend <NNNN> rewrite patch NNNN from the working tree:
                                    its files, against HEAD plus the patches
                                    BEFORE it (a file a later patch also
                                    touches cannot be amended this way)
"""
import os
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
tree = os.path.join(root, 'extern', 'eden')
patches = os.path.join(root, 'patches')


def git(*args, **kw):
    return subprocess.run(['git', '-C', tree] + list(args), check=True,
                          capture_output=True, **kw).stdout


def files_of(p):
    out = set()
    for line in open(os.path.join(patches, p), encoding='utf-8', errors='replace'):
        for prefix in ('--- a/', '+++ b/'):
            if line.startswith(prefix):
                out.add(line[len(prefix):].rstrip('\n'))
    return out


series = sorted(f for f in os.listdir(patches) if f.endswith('.patch'))
amend = None
if sys.argv[1:2] == ['--amend']:
    amend = [p for p in series if p.startswith(sys.argv[2])]
    if len(amend) != 1:
        sys.exit('make-patch: no single patch starts with %s' % sys.argv[2])
    amend = amend[0]
    later = series[series.index(amend) + 1:]
    # a file a later patch also touches keeps this patch's hunks as they are
    # (and must not have been edited beyond the series: that needs a new patch)
    shared = files_of(amend) & set().union(set(), *[files_of(p) for p in later])
    kept = {}
    text = open(os.path.join(patches, amend), 'rb').read()
    for chunk in text.split(b'diff --git a/')[1:]:
        name = chunk.split(b' b/', 1)[0].decode()
        if name in shared:
            kept[name] = b'diff --git a/' + chunk
    series = series[:series.index(amend)]
touched = set()
for p in series:
    touched |= files_of(p)

# changed in the working tree relative to HEAD (untracked included)
changed = set()
for line in git('status', '--porcelain', '--untracked-files=all').decode().splitlines():
    path = line[3:]
    if ' -> ' in path:
        path = path.split(' -> ')[1]
    # a nested submodule (Boost's build writes into external/boost) is not
    # this tree's to patch
    if os.path.isdir(os.path.join(tree, path)):
        continue
    changed.add(path)

with tempfile.TemporaryDirectory() as scratch:
    # the series' state of every file it touches
    for f in touched:
        try:
            data = git('show', 'HEAD:' + f)
        except subprocess.CalledProcessError:
            continue
        os.makedirs(os.path.dirname(os.path.join(scratch, f)) or scratch, exist_ok=True)
        open(os.path.join(scratch, f), 'wb').write(data)
    for p in series:
        subprocess.run(['git', 'apply', os.path.join(patches, p)], cwd=scratch, check=True)

    out = []
    wanted = sorted(files_of(amend)) if amend else sorted(changed | touched)
    for f in wanted:
        if amend and f in kept:
            out.append(kept[f])
            continue
        cur = os.path.join(tree, f)
        if f in touched:
            base = os.path.join(scratch, f)
        else:
            base = os.path.join(scratch, '__head__', f)
            os.makedirs(os.path.dirname(base), exist_ok=True)
            try:
                open(base, 'wb').write(git('show', 'HEAD:' + f))
            except subprocess.CalledProcessError:
                base = None
        have_base = base is not None and os.path.exists(base)
        have_cur = os.path.exists(cur)
        if not have_base and not have_cur:
            continue
        r = subprocess.run(['diff', '-u',
                            '--label', 'a/' + f if have_base else '/dev/null',
                            '--label', 'b/' + f if have_cur else '/dev/null',
                            base if have_base else '/dev/null',
                            cur if have_cur else '/dev/null'],
                           capture_output=True)
        if r.returncode == 1:
            out.append(b'diff --git a/' + f.encode() + b' b/' + f.encode() + b'\n')
            if not have_base:
                out.append(b'new file mode 100644\n')
            out.append(r.stdout)
        elif r.returncode != 0:
            sys.exit(r.stderr.decode())

if not out:
    sys.exit('make-patch: the tree holds nothing beyond the series')
files = [o.split(b'\n', 1)[0].decode().split(' b/', 1)[1].strip() for o in out if o.startswith(b'diff --git')]
if sys.argv[1:] == ['--check']:
    print('\n'.join(files))
    sys.exit(0)
name = amend or sys.argv[1]
path = os.path.join(patches, name if name.endswith('.patch') else name + '.patch')
open(path, 'wb').write(b''.join(out))
print('wrote', path, '-', len(files), 'files:', ', '.join(files))
