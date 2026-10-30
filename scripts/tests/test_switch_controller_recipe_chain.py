#!/usr/bin/env python3
"""Replay every controller recipe from its pinned Git blob, without writing it.

This catches cross-recipe stale anchors even when a host test uses reconstructed
controller support. It does not compile C++ or claim a full target build.
"""
import argparse
import importlib.util
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTROLLER = 'vita3k/util/src/switch_memory_validation.cpp'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-source', action='store_true')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        'controller_recipe_engine', ROOT / 'scripts/apply-switch-write-watch-hooks.py')
    engine = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(engine)
    stages = []
    base = None
    for recipe in engine.RECIPES:
        for item in json.loads(recipe.read_text())['files']:
            if item['path'] != CONTROLLER:
                continue
            require(base is None or base == item['base_blob'],
                    f'{recipe.name}: changed controller base pin')
            base = item['base_blob']
            stages.append((recipe.name, item['replacements']))
    require(base and stages, 'No controller recipe chain found')
    original = subprocess.run(
        ['git', 'cat-file', 'blob', base], cwd=ROOT, check=True,
        capture_output=True).stdout
    require(engine.blob_sha(original) == base, 'Controller Git blob mismatch')
    path = ROOT / CONTROLLER
    current = path.read_bytes() if path.exists() else None
    result = original
    operations = []
    for name, replacements in stages:
        try:
            result = engine.transform(result, replacements)
        except ValueError as exc:
            raise AssertionError(f'{name}: {CONTROLLER}: {exc}') from exc
        operations.extend(replacements)
    require(result == engine.transform(original, operations),
            'Stage-by-stage expansion differs from merged production expansion')

    # The earlier epoch recipe already surfaces domain AND backing/import errors.
    # Do not reapply the stale pre-epoch assignment, or weaken the existing gate.
    text = result.decode('utf-8')
    prefix = '    out.watch_failed = '
    require(text.count(prefix) == 1, 'Missing or duplicate HUD failure assignment')
    assignment = text[text.index(prefix):].split(';', 1)[0]
    for term in ('observation.failed', 'options.epoch_audit',
                 'epoch::domain.snapshot().failed', 'direct_heap::state.errors.load()',
                 'deferred_import::registry.summary().errors'):
        require(term in assignment, f'HUD lost failure source: {term}')

    stale = {'old': '    out.watch_failed = observation.failed;',
             'new': '    out.watch_failed = false;'}
    try:
        engine.transform(original, operations + [stale])
    except ValueError as exc:
        require('expected exactly one' in str(exc), 'Unexpected rejection reason')
    else:
        raise AssertionError('The stale CI #59 anchor was accepted')
    # Duplicate and absent anchors must still fail; no fuzzy or silent rewriting.
    for source in (b'x\nx\n', b'y\n'):
        try:
            engine.transform(source, [{'old': 'x\n', 'new': 'z\n'}])
        except ValueError:
            pass
        else:
            raise AssertionError('Non-unique anchor was accepted')
    if args.require_source:
        require(current == result, 'Production controller is not exactly expanded')
    require((path.read_bytes() if path.exists() else None) == current,
            'Read-only regression modified the production controller')
    print(f'Controller recipe chain PASS: {len(stages)} stages, '
          f'{len(operations)} ordered replacements, pinned base {base}, '
          f'compiled blob {engine.blob_sha(result)}; '
          'stale/duplicate/absent anchors rejected; failure gates retained. '
          'Not a C++/NRO build or device test.')


if __name__ == '__main__':
    main()
