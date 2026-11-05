#!/usr/bin/env python3
"""Reproducibility check: regenerate the PPC corpus from the XEX + recorded configs and compare it with reference trees.

  regen_check.py --generator BUILD/XenonRecomp/XenonRecomp --context SRC/XenonUtils/ppc_context.h \
      --config-from local-analysis/regen/flags-off/recomp.toml --work NEW_PRIVATE_DIR \
      --compare NAME=DIR [--compare NAME=DIR ...] --report REPORT.json [--keep]

* NEW_PRIVATE_DIR must not exist and must lie under ROOT/local-analysis (git-ignored): generated code is game-derived.
* The config is the recorded one with only its three relative paths re-pointed from NEW_PRIVATE_DIR (the XEX, the
  explicit switch tables and the output directory); every other line (helper addresses, function overrides, flags) is
  kept byte for byte. The XEX and the switch tables are read in place, never copied.
* The generator runs once (nice -n 10). Then every ppc_recomp.N.cpp and support file is SHA-256 compared with each
  --compare tree. The report holds counts, chunk numbers and digest prefixes only (no code, no addresses).
* Without --keep the generated ppc/ directory (this run's own output) is removed after hashing; the report and the
  generator log stay.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
import sys
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

ROOT = host_support.package_root(__file__)
LOCAL = ROOT / 'local-analysis'
CHUNK = re.compile(r'ppc_recomp\.(\d+)\.cpp')
SUPPORT = ('ppc_config.h', 'ppc_context.h', 'ppc_func_mapping.cpp', 'ppc_recomp_shared.h')


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def repoint(config: Path, work: Path) -> str:
    base = config.parent
    out = []
    for line in config.read_text(encoding='utf-8').splitlines():
        m = re.fullmatch(r'(file_path|switch_table_file_path) = "(.*)"', line)
        if m:
            target = (base / m.group(2)).resolve()
            if not target.is_file():
                raise SystemExit(f'{m.group(1)} target missing')
            line = f'{m.group(1)} = {json.dumps(host_support.relative_or_absolute(target, work))}'
        elif line.startswith('out_directory_path'):
            line = 'out_directory_path = "./ppc"'
        out.append(line)
    return '\n'.join(out) + '\n'


def tree(directory: Path) -> dict[str, str]:
    names = [p for p in directory.iterdir() if p.is_file() and (CHUNK.fullmatch(p.name) or p.name in SUPPORT)]
    return {p.name: sha(p) for p in names}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--generator', type=Path, required=True)
    ap.add_argument('--context', type=Path, required=True)
    ap.add_argument('--config-from', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--compare', action='append', default=[])
    ap.add_argument('--report', type=Path, required=True)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    work = a.work.absolute()
    if work.exists():
        raise SystemExit('work dir must be new; choose an unused directory')
    host_support.private_output(work)
    host_support.private_output(a.report)
    work.mkdir(parents=True)
    (work / 'ppc').mkdir()
    cfg = work / 'recomp.toml'
    cfg.write_text(repoint(a.config_from.absolute(), work), encoding='utf-8', newline='\n')
    cfg.chmod(0o600)
    t0 = time.monotonic()
    with open(work / 'recompile.log', 'w', encoding='utf-8', newline='\n') as log:
        command, options = host_support.low_priority([str(a.generator), str(cfg), str(a.context)])
        r = subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, **options)
    seconds = round(time.monotonic() - t0, 1)
    if r.returncode != 0:
        raise SystemExit(f'generator failed ({r.returncode}); see {work / "recompile.log"}')
    gen = tree(work / 'ppc')
    report = dict(generator_sha256=sha(a.generator)[:12], context_sha256=sha(a.context)[:12],
                  config_from=str(a.config_from), generate_seconds=seconds,
                  generated_files=len(gen), generated_chunks=sum(1 for n in gen if CHUNK.fullmatch(n)),
                  generated_bytes=sum((work / 'ppc' / n).stat().st_size for n in gen), compare={})
    for spec in a.compare:
        name, d = spec.split('=', 1)
        ref = tree(Path(d))
        same = sorted(n for n in gen if ref.get(n) == gen[n])
        differ = sorted(n for n in gen if n in ref and ref[n] != gen[n])
        report['compare'][name] = dict(directory=d, identical=len(same), different=len(differ),
                                       only_generated=sorted(set(gen) - set(ref)), only_reference=sorted(set(ref) - set(gen)),
                                       different_chunks=sorted(int(CHUNK.fullmatch(n).group(1)) for n in differ if CHUNK.fullmatch(n)),
                                       different_support=[n for n in differ if not CHUNK.fullmatch(n)])
    report['digests'] = {n: h[:12] for n, h in sorted(gen.items())}
    a.report.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    if not a.keep:
        shutil.rmtree(work / 'ppc')
    print(json.dumps({k: (v if k != 'compare' else {n: (c['identical'], c['different']) for n, c in v.items()})
                      for k, v in report.items() if k != 'digests'}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
