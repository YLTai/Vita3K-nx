#!/usr/bin/env python3
"""Replay controller and test-driver recipes from their pinned Git blobs.

Read-only integration regression: catches cross-recipe stale anchors and exact
argument-order mismatches without reconstructed C++ bodies. It does not compile
C++ or claim a full target build.
"""
import argparse
import importlib.util
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTROLLER = 'vita3k/util/src/switch_memory_validation.cpp'
DRIVER = 'scripts/tests/run-switch-performance-tests.sh'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def replay(engine, target, require_source):
    stages = []
    base = None
    for recipe in engine.RECIPES:
        for item in json.loads(recipe.read_text())['files']:
            if item['path'] != target:
                continue
            require(base is None or base == item['base_blob'],
                    f'{recipe.name}: changed base pin for {target}')
            base = item['base_blob']
            stages.append((recipe.name, item['replacements']))
    require(base and stages, f'No recipe chain found: {target}')
    original = subprocess.run(
        ['git', 'cat-file', 'blob', base], cwd=ROOT, check=True,
        capture_output=True).stdout
    require(engine.blob_sha(original) == base, f'Git blob mismatch: {target}')
    path = ROOT / target
    current = path.read_bytes() if path.exists() else None
    result = original
    operations = []
    for name, replacements in stages:
        try:
            result = engine.transform(result, replacements)
        except ValueError as exc:
            raise AssertionError(f'{name}: {target}: {exc}') from exc
        operations.extend(replacements)
    require(result == engine.transform(original, operations),
            f'Staged and merged expansion differ: {target}')
    if require_source:
        require(current == result, f'Production source not exactly expanded: {target}')
    require((path.read_bytes() if path.exists() else None) == current,
            f'Read-only regression modified source: {target}')
    print(f'Pinned chain PASS: {target}: {len(stages)} stages / '
          f'{len(operations)} replacements, expanded blob {engine.blob_sha(result)}')
    return original, operations, result


def rejected(engine, original, operations, old, reason):
    try:
        engine.transform(original, operations + [{'old': old, 'new': 'invalid'}])
    except ValueError as exc:
        require('expected exactly one' in str(exc), 'Unexpected rejection reason')
    else:
        raise AssertionError(f'Stale {reason} anchor was accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-source', action='store_true')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        'controller_recipe_engine', ROOT / 'scripts/apply-switch-write-watch-hooks.py')
    engine = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(engine)
    original, operations, controller = replay(engine, CONTROLLER, args.require_source)

    # Earlier epoch recipes already preserve all these failure sources.
    text = controller.decode('utf-8')
    prefix = '    out.watch_failed = '
    require(text.count(prefix) == 1, 'Missing or duplicate HUD failure assignment')
    assignment = text[text.index(prefix):].split(';', 1)[0]
    for term in ('observation.failed', 'options.epoch_audit',
                 'epoch::domain.snapshot().failed', 'direct_heap::state.errors.load()',
                 'deferred_import::registry.summary().errors'):
        require(term in assignment, f'HUD lost failure source: {term}')
    rejected(engine, original, operations, '    out.watch_failed = observation.failed;', 'CI #59')

    original, operations, driver = replay(engine, DRIVER, args.require_source)
    text = driver.decode('utf-8')
    gxm = 'python3 "$ROOT/scripts/tests/test_switch_gxm_contracts.py" --require-source --cxx "${CXX:-c++}"'
    chain = 'python3 "$ROOT/scripts/tests/test_switch_controller_recipe_chain.py" --require-source'
    outputs = 'python3 "$ROOT/scripts/tests/test_switch_host_outputs.py" --cxx "${CXX:-c++}" --require-source'
    for command in (gxm, chain, outputs):
        require(text.splitlines().count(command) == 1, f'Missing/duplicate test: {command}')
    require(text.index(gxm) < text.index(chain) < text.index(outputs), 'Wrong test order')
    rejected(engine, original, operations,
             'python3 "$ROOT/scripts/tests/test_switch_gxm_contracts.py" --cxx "${CXX:-c++}" --require-source',
             'CI #60 argument-order')
    # No fuzzy or silent rewriting, including duplicate and absent anchors.
    for source in (b'x\nx\n', b'y\n'):
        try:
            engine.transform(source, [{'old': 'x\n', 'new': 'z\n'}])
        except ValueError:
            pass
        else:
            raise AssertionError('Non-unique anchor was accepted')
    print('Controller and test-driver chains PASS; CI #59/#60 negatives rejected; '
          'failure gates and single test invocations retained. Not a C++/NRO or device test.')


if __name__ == '__main__':
    main()
