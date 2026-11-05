"""Connect generated private shader headers to the native compile input tree."""
import hashlib
import json
from pathlib import Path

SHADER_INCLUDES=('generic_denorm_overrides.inc','generic_gradient_overrides.inc',
                 'generic_reglod_overrides.inc','generic_container_swizzles.inc')
# build_shader_fixes.py's tables (the renderer's later fixes: Thor line, attempts v59x and later)
SHADER_FIXES=('generic_ptx_overrides.inc','generic_cloth_overrides.inc',
              'generic_coissue_overrides.inc','generic_coissue_reglod_overrides.inc')


def install_shader_inputs(generated,source,names=SHADER_INCLUDES):
    generated=Path(generated).resolve(strict=True);source=Path(source)
    report=json.loads((generated/'report.json').read_text())
    if report.get('status')!='complete' or set(report.get('outputs',{}))!=set(names):
        raise ValueError('complete generated shader include receipt required')
    pending=[]
    for name in names:
        path=generated/name
        if path.is_symlink():raise ValueError('generated shader symlink refused')
        blob=path.read_bytes();digest=hashlib.sha256(blob).hexdigest();expected=report['outputs'][name]
        if len(blob)!=expected['bytes'] or digest!=expected['sha256']:
            raise ValueError('generated shader producer identity refused')
        if (source/name).exists():raise ValueError('resident shader include must be excluded before clone')
        pending.append((name,blob,digest))
    for name,blob,digest in pending:
        with (source/name).open('xb') as stream:stream.write(blob)
        if hashlib.sha256((source/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('compile shader consumer identity refused')
    return dict(source=str(generated),report_sha256=hashlib.sha256((generated/'report.json').read_bytes()).hexdigest(),
                files={name:dict(bytes=len(blob),sha256=digest) for name,blob,digest in pending})
