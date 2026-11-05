"""Restricted MUL/ADD/SUB_CONST operand decoding, not full ALU execution.

Encoding cross-check: Xenia 95a5c3ee gpu/ucode.h scalar conventions and local
XenosRecomp shader_recompiler.cpp. The opcode's low bit is temporary-index bit0;
src3_sel is bit1 and swizzle[2:5] are bits[2:5]. Source3 negation affects BOTH
operands. Relative/absolute addressing, scalar clamp and export are rejected.
"""
from raw_shader_analysis import RawShaderError


def decode_scalar_constant(words, *, coissued=False, allow_clamp=False, allow_export=False):
    if len(words) != 3 or any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in words):
        raise RawShaderError('three scalar ALU dwords required')
    a, b, c = words
    opcode = a >> 26
    forbidden_a = (1 << 6) | (1 << 7)
    if not allow_export:
        # Bit 15 is the export itself. Bit 14 only means anything on one: it
        # zero-fills the lanes the vector and scalar masks leave unwritten, and
        # BOTH parsers that call this already apply that and already reject the
        # bit outright on a non-export, before this decoder ever runs. Forbidding
        # it here as well refuses a valid export for a field its caller has
        # handled; off an export it stays forbidden, as it was.
        forbidden_a |= (1 << 15) | (1 << 14)
    if not allow_clamp:
        forbidden_a |= 1 << 25
    if not coissued:
        forbidden_a |= 1 << 24
    allowed_b = 0x07FFFFFF if coissued else 0x00FFFFFF
    if opcode not in range(42, 48) or a & forbidden_a or b & ~allowed_b:
        raise RawShaderError('unsupported bounded MUL/ADD/SUB_CONST fields')
    return {'operation': ('mul','add','sub')[(opcode - 42) // 2],
            'constant_register': c & 255, 'constant_component': ((b >> 6) + 3) & 3,
            'temporary_register': (opcode & 1) | (((c >> 29) & 1) << 1) | (b & 60),
            'temporary_component': b & 3, 'destination_register': (a >> 8) & 63,
            'write_mask': (a >> 20) & 15, 'negate_both': bool(b & (1 << 24)),
            'destination_clamped': bool(a & (1 << 25)),
            # Only operand decoding. The containing stage must independently
            # validate allocation, export slot, scalar/vector masks and writes.
            'export': bool(a & (1 << 15)),
            'previous_scalar_updated': True, 'vector_executed': False, 'gpu_executed': False}


def scalar_constant_node(plan, registers, node):
    left = node('constant', plan['constant_register'], plan['constant_component'])
    right = registers[plan['temporary_register']][plan['temporary_component']]
    if plan['negate_both']:
        left, right = node('negate', left), node('negate', right)
    if plan['operation'] == 'sub':
        right = node('negate', right)
    return node('mul' if plan['operation'] == 'mul' else 'add', left, right)
