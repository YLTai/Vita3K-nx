#!/usr/bin/env python3
"""Apply pinned experimental hooks transactionally and idempotently.

The shared observer is normal source code; this small recipe isolates the
experimental integration into mem, CPU callbacks and texture hashing. Pinning
ENTIRE original Git blobs prevents fuzzy application onto changed semantics.
Preflight and CMake use --check-applied. Never fetch code from the network.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECIPE = ROOT / 'patches/experiments/switch-live-write-watch.json'
RECIPES = (RECIPE, ROOT / 'patches/experiments/switch-heap-fault.json',
           ROOT / 'patches/experiments/switch-direct-heap.json',
           ROOT / 'patches/experiments/switch-deferred-import.json',
           ROOT / 'patches/experiments/switch-mirror-shader.json',
           ROOT / 'patches/experiments/switch-epoch-audit.json',
           ROOT / 'patches/experiments/switch-scoped-hle.json',
           ROOT / 'patches/experiments/switch-native-waits.json',
           ROOT / 'patches/experiments/switch-gxm-contracts.json')


def blob_sha(data):
    return hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()


def transform(original, replacements):
    text = original.decode('utf-8')
    for item in replacements:
        old, new = item['old'], item['new']
        if not old or text.count(old) != 1:
            raise ValueError('expected exactly one matching source anchor: ' + old[:100])
        text = text.replace(old, new, 1)
    return text.encode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-applied', action='store_true')
    args = parser.parse_args()
    # An ordered extension must pin the SAME original blob. Merge anchors and
    # transform once from that original; never patch an arbitrary intermediate.
    merged = {}
    for recipe in RECIPES:
        for item in json.loads(recipe.read_text())['files']:
            key = item['path']
            if key in merged:
                if merged[key]['base_blob'] != item['base_blob']:
                    raise ValueError(f'{key}: extension changed the original blob pin')
                merged[key]['replacements'].extend(item['replacements'])
            else:
                merged[key] = dict(item, replacements=list(item['replacements']))
    specs = list(merged.values())
    pending, provenance = [], []
    for spec in specs:
        rel = Path(spec['path'])
        if rel.is_absolute() or '..' in rel.parts:
            raise ValueError('unsafe recipe path')
        original = subprocess.run(['git', 'cat-file', 'blob', spec['base_blob']],
                                  cwd=ROOT, check=True, capture_output=True).stdout
        if blob_sha(original) != spec['base_blob']:
            raise ValueError('base object identity mismatch')
        result = transform(original, spec['replacements'])
        path = ROOT / rel
        current = path.read_bytes()
        if current != result:
            if args.check_applied or current != original:
                raise ValueError(f'{rel}: unrecognized or unapplied hooks; inspect before building')
            pending.append((path, result))
        provenance.append(dict(path=rel.as_posix(), base_blob=spec['base_blob'],
                               compiled_blob=blob_sha(result)))
    # Validate all files before modifying any. Interrupted application is safe
    # to rerun because both pristine and exact expanded states are accepted.
    for path, result in pending:
        path.write_bytes(result)
    if not args.check_applied:
        dest = ROOT / 'build-switch-preflight/write-watch-hooks.json'
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(dict(recipe_sha256=hashlib.sha256(b''.join(p.read_bytes() for p in RECIPES)).hexdigest(),
            recipes=[dict(path=p.relative_to(ROOT).as_posix(), sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in RECIPES],
            files=provenance), indent=2) + '\n')
    print('Live write-watch hooks: exact pinned source and compiled identities verified')


if __name__ == '__main__':
    main()
