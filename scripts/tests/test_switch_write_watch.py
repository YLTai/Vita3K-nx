#!/usr/bin/env python3
"""Compile and run the real observer; validate exact integration recipe rules."""
import argparse
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(); p.add_argument('--cxx', default='c++'); args = p.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        exe = str(Path(directory) / 'watch')
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-I', str(ROOT/'vita3k/util/include'), str(ROOT/'vita3k/util/tests/switch_write_watch.cpp'),
                        '-o', exe], check=True)
        subprocess.run([exe], check=True, timeout=20)
    spec = importlib.util.spec_from_file_location('hooks', ROOT/'scripts/apply-switch-write-watch-hooks.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    assert mod.transform(b'abc\n', [dict(old='abc', new='xyz')]) == b'xyz\n'
    for text in (b'absent', b'abc abc'):
        try: mod.transform(text, [dict(old='abc', new='xyz')])
        except ValueError: pass
        else: raise AssertionError('ambiguous / missing anchor was accepted')
    data = json.loads((ROOT/'patches/experiments/switch-live-write-watch.json').read_text())
    assert len(data['files']) == 5
    all_new = '\n'.join(r['new'] for f in data['files'] for r in f['replacements'])
    for required in ['MappingGuard', 'publish_page_entry', 'live_detach_table', 'live_read', 'live_write(',
                     'live_exclusive', 'config.write_page_table', '16 | 32 | 64 | 128', 'live_hash_begin', 'live_hash_end', 'palette_count']:
        assert required in all_new, required
    # No sampled-Hash authority, code invalidation or forced all-store callback.
    assert 'config.page_table = nullptr' not in all_new
    print('Live write observer: aliases, cross-pages, atomics, remap, bounds, metadata concurrency, fail-closed anchors OK')

if __name__ == '__main__': main()
