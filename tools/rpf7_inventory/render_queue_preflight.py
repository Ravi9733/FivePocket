"""Inventory captured queued handler families without predicting execution."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from xenos_preflight_batch import BatchLimits, _require_private_input, _read_private_input


def queue_requirements(meta, payloads):
    if not isinstance(meta, dict) or meta.get('version') != 1 or meta.get('execution_proven') is not False:
        raise ValueError('diagnostic queue version1 required; execution must be unproven')
    def integer(value):
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise ValueError('unsigned captured word required')
        return value
    root, head = integer(meta.get('root')), integer(meta.get('entry_handle'))
    records = meta.get('records')
    if not isinstance(records, list) or len(records) > 128:
        raise ValueError('bounded queue records required')
    issues, unavailable, handlers, decoded, total = [], [], {}, [], 0
    expected_head = head
    for i, record in enumerate(records):
        if not isinstance(record, dict): raise ValueError('queue record object required')
        fields = {k: integer(record.get(k)) for k in (
            'handle', 'bank_pointer', 'bank_base', 'address', 'word', 'advance', 'handler_slot', 'payload_bytes')}
        handler = record.get('handler')
        if handler is not None: integer(handler)
        handle, word = fields['handle'], fields['word']
        advance, slot = ((word >> 20) & 4095)*4, (word >> 12) & 255
        if (handle != expected_head or handle >> 24 >= 128 or fields['bank_pointer'] != root+1032+20*(handle >> 24)
                or fields['address'] != fields['bank_base']+(handle & 0xFFFFFF)
                or fields['address'] % 4 or fields['advance'] != advance or fields['handler_slot'] != slot):
            issues.append(f'record {i}: metadata disagrees with original decoder')
        if i+1 < len(records) and (slot == 2 or not advance):
            issues.append(f'record {i}: inventory continued past an unknown/terminal linear boundary')
        expected_head = handle+advance
        name = f'queue-record-{i}.bin'; body = payloads.get(name)
        if body is None or not len(body):
            unavailable.append(name)
        elif len(body) != max(4, advance) or len(body) != fields['payload_bytes']:
            issues.append(f'record {i}: body length mismatch')
        elif struct.unpack_from('>I', body)[0] != word:
            issues.append(f'record {i}: body/header mismatch')
        total += len(body) if body is not None else 0
        if total > 256*1024: raise ValueError('queue payload budget exceeded')
        if handler is None: unavailable.append(f'record {i}: handler pointer')
        elif handler: handlers.setdefault(f'{handler:08X}', []).append(i)
        row = dict(index=i, handler=handler, advance=advance, slot=slot)
        # This adapter's scalar argument offsets are proved in original822F6500.
        # They are snapshot-time values, not effective future bindings/arguments.
        if handler == 0x822F6500 and body is not None and len(body) >= 20:
            depth_bits = struct.unpack_from('>I', body, 8)[0]
            row['queued_clear_adapter'] = dict(packed_color=struct.unpack_from('>I', body, 4)[0],
                depth_float32_bits=depth_bits, stencil=struct.unpack_from('>I', body, 12)[0], flags_byte=body[16])
        decoded.append(row)
    return dict(classification='read-only potential fallthrough inventory, NOT native admission or future execution',
                issues=issues, unavailable=unavailable, stop=meta.get('stop'), record_count=len(records),
                handler_families=handlers, suggested_codegen_roots=sorted(handlers), records=decoded,
                unverified=['current callback and which records already executed',
                            'callback-modified head, records, handlers, bindings and pointed resources',
                            'nonlinear branches and records outside capture budgets',
                            'native renderer support, GPU completion and gameplay'],
                payload_sha256={k: hashlib.sha256(v).hexdigest() for k, v in payloads.items() if v is not None})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args(); root = Path(__file__).resolve().parents[2]
    limits = BatchLimits(max_file_bytes=65536)
    def read(name, optional=False):
        path = args.capture/name
        if optional and not path.exists() and not path.is_symlink(): return None
        return _read_private_input(_require_private_input(path, root, limits))
    meta_bytes = read('render-queue.json'); meta = json.loads(meta_bytes)
    queue_requirements(meta, {})  # Validate bounded metadata before selecting files.
    payloads = {f'queue-record-{i}.bin': read(f'queue-record-{i}.bin', True) for i in range(len(meta['records']))}
    report = queue_requirements(meta, payloads)
    report['metadata_sha256'] = hashlib.sha256(meta_bytes).hexdigest()
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__': main()
