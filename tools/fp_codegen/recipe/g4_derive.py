#!/usr/bin/env python3
"""g4_derive.py: derive the parameters that turn XenonRecomp output into the validated corpus (design gap G4).

  g4_derive.py GEN_DIR CORPUS_DIR OUT_DIR --chunks @FILE --image IMAGE --map FUNC_MAPPING.cpp
               --image-base B --code-base C --code-size S [--base-switch-toml T]

GEN_DIR = the 19-patch generator output (regen_check.py --keep), CORPUS_DIR = the validated corpus, FILE = the chunks
whose corpus text differs from GEN_DIR. OUT_DIR (new, private, under local-analysis/) receives:
  switch-scan.json, switch-scan-image.json   switch_recover.py scan + verify-image (independent image idiom check)
  switch-expand.json                         switch_expand.py records   (addresses only)
  switch_tables.recovered.toml               T + the recovered tables in the generator's schema (for a regeneration check)
  case-blocks.json                           case_blocks.py records      (addresses only; bodies copied from GEN_DIR)
  error-sites.json                           error_sites.py records      (addresses; recorded bodies where the corpus's
                                                                         text is not the generator's own translation)
  recorded-hunks.json                        recorded_hunks.py records   (exact one-off edits; generated code)
  scope.json                                 the chunk list (scope of the parameterless passes)
  report.json                                per chunk: passes that change it, reproduced or not, closing kind
The chain, in order: case_blocks, switch_expand, error_sites, vmx_unpack_call, vmx_helper_includes, eof_blank,
recorded_hunks. Everything is computed from the two trees (and the image for the check only); no game data in this tool.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import case_blocks  # noqa: E402
import eof_blank  # noqa: E402
import error_sites  # noqa: E402
import recorded_hunks  # noqa: E402
import switch_expand  # noqa: E402
import switch_recover  # noqa: E402
import textpass  # noqa: E402
import vmx_helper_includes  # noqa: E402
import vmx_unpack_call  # noqa: E402

CHAIN = ('case_blocks', 'switch_expand', 'error_sites', 'vmx_unpack_call', 'vmx_helper_includes', 'eof_blank',
         'recorded_hunks')


def chain_apply(n: int, text: str, params: dict, upto: str | None = None) -> tuple[str, list[str]]:
    """apply the chain to chunk n; returns the text and the ids of the passes that changed it"""
    key, used = str(n), []
    for name in CHAIN:
        if name == upto:
            break
        old = text
        if name == 'case_blocks' and key in params['case_blocks']:
            text = case_blocks.apply(text, params['case_blocks'][key])[0]
        elif name == 'switch_expand' and key in params['switch_expand']:
            text = switch_expand.apply(text, params['switch_expand'][key])[0]
        elif name == 'error_sites' and key in params['error_sites']:
            text = error_sites.apply(text, params['error_sites'][key])[0]
        elif name == 'vmx_unpack_call':
            text = vmx_unpack_call.apply(text)[0]
        elif name == 'vmx_helper_includes':
            text = vmx_helper_includes.apply(text)[0]
        elif name == 'eof_blank':
            text = eof_blank.apply(text)[0]
        elif name == 'recorded_hunks' and key in params.get('recorded_hunks', {}):
            text = recorded_hunks.apply(text, params['recorded_hunks'][key])[0]
        if text != old:
            used.append(name)
    return text, used


def write_json(path: Path, obj: dict) -> None:
    if path.exists():
        raise SystemExit(f'{path} exists')
    path.write_text(json.dumps(obj, indent=1) + '\n', encoding='utf-8', newline='\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('gen'); ap.add_argument('corpus'); ap.add_argument('out')
    ap.add_argument('--chunks', required=True); ap.add_argument('--image', required=True); ap.add_argument('--map', required=True)
    ap.add_argument('--image-base', required=True); ap.add_argument('--code-base', required=True)
    ap.add_argument('--code-size', required=True); ap.add_argument('--base-switch-toml')
    a = ap.parse_args()
    out = Path(a.out)
    if out.exists():
        raise SystemExit(f'{out} exists')
    out.mkdir(parents=True)
    spec = Path(a.chunks[1:]).read_text() if a.chunks.startswith('@') else a.chunks
    chunks = [int(x) for x in re.split(r'[\s,]+', spec) if x]
    clist = ','.join(map(str, chunks))
    gen, corpus = Path(a.gen), Path(a.corpus)
    # 1. switch tables: recover from the corpus code, check against the image idiom scanners, write records (+ toml)
    switch_recover.cmd_scan(SimpleNamespace(gen=str(gen), code=str(corpus), out=str(out / 'switch-scan.json'), chunks=clist))
    switch_recover.cmd_verify_image(SimpleNamespace(scan=str(out / 'switch-scan.json'), out=str(out / 'switch-scan-image.json'),
                                                    image=a.image, map=a.map, image_base=a.image_base,
                                                    code_base=a.code_base, code_size=a.code_size))
    switch_recover.cmd_records(SimpleNamespace(scan=str(out / 'switch-scan-image.json'), out=str(out / 'switch-expand.json')))
    if a.base_switch_toml:
        switch_recover.cmd_toml(SimpleNamespace(base=a.base_switch_toml, scan=str(out / 'switch-scan-image.json'),
                                                out=str(out / 'switch_tables.recovered.toml'),
                                                forms='live,compact,compact-default', allow_no_default=True))
    # 2. short cases and 3. leaf/tail sites
    case_blocks.derive(SimpleNamespace(gen=str(gen), code=str(corpus), out=str(out / 'case-blocks.json'), chunks=clist))
    error_sites.derive(SimpleNamespace(gen=str(gen), code=str(corpus), out=str(out / 'error-sites.json'), chunks=clist))
    params = {'case_blocks': json.loads((out / 'case-blocks.json').read_text())['chunks'],
              'switch_expand': json.loads((out / 'switch-expand.json').read_text())['chunks'],
              'error_sites': json.loads((out / 'error-sites.json').read_text())['chunks']}
    # 4. structured chain; what remains becomes recorded hunks
    hunks, rows = {}, {}
    for n in chunks:
        g = textpass.read_text(gen / f'ppc_recomp.{n}.cpp')
        c = textpass.read_text(corpus / f'ppc_recomp.{n}.cpp')
        t, used = chain_apply(n, g, params, upto='recorded_hunks')
        if t != c:
            hunks[str(n)] = recorded_hunks.derive(t, c)
    params['recorded_hunks'] = hunks
    write_json(out / 'recorded-hunks.json', {'schema': 'gta5-recorded-hunks/1', 'note': 'private: exact one-off edits of '
                                             'generated code (recorded_hunks.py)', 'chunks': hunks})
    write_json(out / 'scope.json', {'schema': 'gta5-g4-scope/1', 'chunks': chunks})
    # 5. verify the whole chain per chunk
    kinds: dict[str, int] = {}
    for n in chunks:
        g = textpass.read_text(gen / f'ppc_recomp.{n}.cpp')
        c = textpass.read_text(corpus / f'ppc_recomp.{n}.cpp')
        t, used = chain_apply(n, g, params)
        ok = t == c
        kind = ('not reproduced' if not ok else 'recorded hunks' if 'recorded_hunks' in used else
                'recorded leaf/tail bodies' if str(n) in params['error_sites'] and any(
                    not b.get('constant') for b in params['error_sites'][str(n)]['bodies'].values()) else
                'derived (addresses only / no parameters)')
        kinds[kind] = kinds.get(kind, 0) + 1
        rows[str(n)] = {'passes': used, 'reproduced': ok, 'kind': kind,
                        'sha256': hashlib.sha256(t.encode()).hexdigest()[:16]}
    write_json(out / 'report.json', {'schema': 'gta5-g4-derive/1', 'generator_dir': str(gen), 'corpus_dir': str(corpus),
                                     'chain': list(CHAIN), 'kinds': kinds, 'chunks': rows})
    print(json.dumps(kinds))
    return 0 if all(r['reproduced'] for r in rows.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
