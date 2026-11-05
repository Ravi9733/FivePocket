"""Opt-in bounded private PM4 snapshots. Framing validation is not execution."""
import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile

MAX_CAPTURE_WORDS = 4096
MAX_CAPTURE_BYTES = MAX_CAPTURE_WORDS * 4
MAX_RECORD_CHARS = MAX_CAPTURE_BYTES * 2 + 8

def decode_packets(data, *, max_capture_words=MAX_CAPTURE_WORDS):
    # Larger request-local binary diagnostics opt in explicitly. The stdout
    # protocol capture retains its original default and aggregate limits.
    if (type(max_capture_words) is not int or not 1 <= max_capture_words <= 256*1024 or
            len(data) % 4 or len(data) > max_capture_words*4):
        raise ValueError('invalid PM4 byte extent')
    words = struct.unpack('>' + 'I' * (len(data) // 4), data)
    packets, offset = [], 0
    while offset < len(words):
        header = words[offset]
        kind = header >> 30
        size = ((header >> 16) & 16383) + 1 if kind in (0, 3) else (2 if kind == 1 else 0)
        if offset + 1 + size > len(words):
            raise ValueError('truncated PM4 packet')
        packets.append([offset, kind, size, (header >> 8) & 127 if kind == 3 else 0,
            header & 32767 if kind == 0 else (header & 2047 if kind == 1 else 0),
            (header >> 11) & 2047 if kind == 1 else 0,
            header & 1 if kind == 3 else 0, (header >> 15) & 1 if kind == 0 else 0])
        offset += 1 + size
    return words, packets


def parse_pm4_capture(status, records, root, graph):
    if status is None:
        if records: raise ValueError('PM4 blobs require explicit capture status')
        return None
    if status not in ('0', '1', '2') or len(records) > 17:
        raise ValueError('invalid PM4 capture bounds')
    if status != '1':
        if records: raise ValueError('unavailable PM4 capture cannot publish blobs')
        return {'status': int(status), 'blobs': []}
    if root is None or root['status'] != 5 or graph is None or graph['failure']:
        raise ValueError('PM4 capture requires a complete inspected graph')
    nodes = [root, *graph['nodes']]
    if len(nodes) != len(records) or graph['total_words'] > MAX_CAPTURE_WORDS:
        raise ValueError('PM4 capture incomplete or excessive')
    blobs, total = [], 0
    for index, (raw, metadata) in enumerate(zip(records, nodes)):
        if len(raw) > MAX_RECORD_CHARS or not re.fullmatch(r'(?:0|[1-9][0-9]?),[0-9a-f]*', raw):
            raise ValueError('invalid private PM4 record')
        node, encoded = raw.split(',')
        if int(node) != index or len(encoded) != metadata['word_count'] * 8:
            raise ValueError('PM4 capture does not match node extent')
        code = bytes.fromhex(encoded)
        total += len(code)
        if total > MAX_CAPTURE_BYTES: raise ValueError('excessive PM4 capture bytes')
        _, packets = decode_packets(code)
        if (len(packets) != metadata['packet_count'] or
                packets[:len(metadata['packets'])] != metadata['packets']):
            raise ValueError('captured PM4 framing does not match inspected metadata')
        blobs.append({'node': index, 'bytes': code})
    if total != graph['total_words'] * 4:
        raise ValueError('PM4 capture aggregate mismatch')
    children = {}
    for index, metadata in enumerate(graph['nodes'], 1):
        parent, offset = metadata['parent'], metadata['parent_offset']
        if not 0 <= parent < index or (parent, offset) in children:
            raise ValueError('invalid or duplicate captured parent')
        words, packets = decode_packets(blobs[parent]['bytes'])
        packet = next((p for p in packets if p[0] == offset), None)
        if (packet is None or (packet[1], packet[2], packet[3], packet[6]) not in
                ((3, 2, 63, 0), (3, 2, 55, 0)) or
                words[offset + 1:offset + 3] != (metadata['physical_address'], metadata['word_count'])):
            raise ValueError('captured indirect pointer does not match inspected child')
        children[parent, offset] = index
    # No unrepresented indirect call may hide after packet-retention truncation.
    for blob in blobs:
        for packet in decode_packets(blob['bytes'])[1]:
            if packet[1] == 3 and packet[3] in (55, 63) and (blob['node'], packet[0]) not in children:
                raise ValueError('incomplete captured indirect graph')
    return {'status': 1, 'blobs': blobs}


def ordered_packets(capture, graph):
    """Expand already validated IB occurrences in packet order, without execution.

Conditional non-IB commands remain opaque yielded packets. A semantic analyzer
must reject them before interpreting subsequent state as unconditional.
"""
    if capture['status'] != 1: raise ValueError('complete PM4 capture required')
    children = {(n['parent'], n['parent_offset']): i for i, n in enumerate(graph['nodes'], 1)}
    decoded = [decode_packets(blob['bytes']) for blob in capture['blobs']]
    def walk(node, depth):
        if depth > 8: raise ValueError('captured traversal depth exceeded')
        words, packets = decoded[node]
        for packet in packets:
            child = children.get((node, packet[0]))
            if child is not None:
                yield from walk(child, depth + 1)
            else:
                yield node, packet, words[packet[0] + 1:packet[0] + packet[2] + 1]
    yield from walk(0, 0)


def save_pm4_capture(output_directory, capture):
    if capture is None: return None
    report = {'status': capture['status'], 'payloads_in_report': False, 'executed': False, 'artifacts': []}
    if capture['status'] != 1 or not capture['blobs']: return report
    # output_directory is already restricted to ignored local-analysis by the
    # orchestrator. Never overwrite previous captures or follow per-file links.
    directory = Path(tempfile.mkdtemp(prefix='stopped-pm4-', dir=output_directory))
    for blob in capture['blobs']:
        path = directory / f"node-{blob['node']}.pm4.bin"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        with os.fdopen(fd, 'wb') as stream: stream.write(blob['bytes'])
        report['artifacts'].append({'node': blob['node'], 'path': str(path.relative_to(output_directory)),
            'word_count': len(blob['bytes']) // 4, 'sha256': hashlib.sha256(blob['bytes']).hexdigest()})
    return report
