"""Opt-in straight-line loading-shader math, not hardware equivalence.

Encoding: Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec gpu/ucode.h,
AluScalarOpcode and AluVectorOpcode. Independent finite-normal/zero CPU policy;
transcendental approximations and exceptional inputs require separate evidence.
PRED_SETNE scalar results are separate opt-in; the pixel parser owns guarded
execution/predicate state. No address-register mutation, kill or memory exports.
"""
import math
import struct
from raw_shader_analysis import RawShaderError


LOADING_OPERATIONS = frozenset(('sqrt', 'rsqrt', 'exp2', 'log2', 'greater', 'greater_equal', 'nonzero', 'truncate', 'floor', 'fraction', 'select_gt0', 'maximum', 'minimum', 'absolute', 'sine', 'cosine'))


def loading_scalar_node(words, registers, node, previous, *, predicate_profile=False, geometry_profile=False,
                        predicate_ge_profile=False, composite_profile=False, tonemap_profile=False,
                        scalar_gt_zero=False, filter_weights_profile=False):
    a, b, c = words
    opcode = a >> 26
    if opcode not in (0, 1, 2, 3, 5, 6, 12, 14, 19, 22, 25, 40) and not (
            (predicate_profile and (opcode==28 or (predicate_ge_profile and opcode==30))) or
            (geometry_profile and opcode in (8,10,11,16)) or
            (scalar_gt_zero and opcode==8) or
            (filter_weights_profile and opcode==13) or
            (composite_profile and opcode==11) or
            # SIN/COS are Xenia AluScalarOpcode 48/49. They are the first
            # periodic transcendentals admitted here, so they stay behind their
            # own profile rather than joining the geometry set; FRACs and LOG
            # come with them because the captured pass uses all four together.
            (tonemap_profile and opcode in (11,16,48,49))):
        raise RawShaderError('unsupported loading scalar operation')
    source, swizzle, temporary = c & 255, b & 255, bool(c & (1 << 29))
    if temporary and source & 64:
        raise RawShaderError('relative/absolute loading scalar source unsupported')
    def operand(lane):
        result = registers[source & 63][lane] if temporary else node('constant', source, lane)
        if temporary and source & 128:
            result = node('absolute', result)
        return node('negate', result) if b & (1 << 24) else result
    left = operand(((swizzle >> 6) + 3) & 3)
    if opcode == 28:
        return node('predicate_setne', left)
    if opcode == 30:
        # PRED_SETGTE returns scalar0/p0=true for src>=0, including both zeros.
        # The pixel parser publishes p0 separately from destination write masks.
        return node('predicate_setge', left)
    if opcode == 8:
        return node('greater', left, node('literal',0))
    if opcode == 10:
        return node('nonzero', left)
    if opcode == 11:
        return node('fraction', left)
    if opcode == 13:
        return node('floor', left)
    if opcode in (12, 14, 16, 19, 22, 40, 48, 49):
        return node({12:'truncate', 14:'exp2', 16:'log2', 19:'reciprocal', 22:'rsqrt', 40:'sqrt',
                     48:'sine', 49:'cosine'}[opcode], left)
    if opcode in (1, 3):
        if previous is None:
            raise RawShaderError('unknown loading scalar previous result')
        return node('add' if opcode==1 else 'mul', left, previous)
    right = operand(swizzle & 3)
    if opcode == 5:
        return node('maximum', left, right)
    # MINs mirrors MAXs. Xenia selects src0 when it compares strictly less, so
    # the tie keeps src1 exactly as MAXs keeps src0; do not fold to a library
    # min() whose tie and NaN behaviour is not the one being reproduced.
    if opcode == 6:
        return node('minimum', left, right)
    if opcode == 25:
        return node('add', left, node('negate', right))
    if opcode == 0:
        return node('add', left, right)
    return node('mul', left, right)


def evaluate_loading_operation(operation, operands):
    """Explicit finite normal/zero policy, not an Xbox approximation oracle."""
    arity = {'sqrt':1, 'rsqrt':1, 'exp2':1, 'log2':1, 'greater':2, 'greater_equal':2, 'nonzero':1, 'truncate':1, 'floor':1, 'fraction':1, 'select_gt0':3, 'maximum':2, 'minimum':2, 'absolute':1, 'sine':1, 'cosine':1}
    if operation not in arity or len(operands) != arity[operation]:
        raise RawShaderError('invalid loading operation or arity')
    for value in operands:
        if not math.isfinite(value) or (value != 0 and abs(value) < 2**-126):
            raise RawShaderError('loading math requires finite normal-or-zero inputs')
        try:
            rounded = struct.unpack('>f', struct.pack('>f', value))[0]
        except (OverflowError, struct.error) as error:
            raise RawShaderError('loading input outside float32') from error
        if rounded != value:
            raise RawShaderError('loading math requires explicit float32 inputs')
    left = operands[0]
    try:
        if operation == 'sqrt':
            if left < 0: raise RawShaderError('negative square root unsupported')
            result = math.sqrt(left)
        elif operation == 'rsqrt':
            if left <= 0: raise RawShaderError('nonpositive reciprocal square root unsupported')
            result = 1.0 / math.sqrt(left)
        elif operation == 'exp2': result = 2.0 ** left
        elif operation == 'log2':
            # LOG_IEEE scalar16, not LOG_CLAMP15. The bounded reference has
            # no Inf/NaN propagation policy; zero/negative values must reject,
            # never be clamped to a fabricated finite value.
            if left <= 0: raise RawShaderError('nonpositive logarithm outside finite geometry policy')
            result = math.log2(left)
        # Xenos SIN/COS take radians, as Xenia's kSin/kCos do. The argument is
        # held to the same finite-normal float32 discipline as every other
        # operand here, and |x| is bounded so that a huge argument cannot make
        # the host's own range reduction the thing being reproduced. This is a
        # bounded CPU policy, NOT a claim about Xbox transcendental precision.
        elif operation in ('sine', 'cosine'):
            if abs(left) > 2.0 ** 24:
                raise RawShaderError('periodic argument outside bounded range-reduction policy')
            result = math.sin(left) if operation == 'sine' else math.cos(left)
        elif operation == 'absolute': result = abs(left)
        elif operation == 'greater': result = float(left > operands[1])
        elif operation == 'greater_equal': result = float(left >= operands[1])
        elif operation == 'truncate': result = math.copysign(float(math.trunc(left)),left)
        elif operation == 'floor': result = left if left==0 else float(math.floor(left))
        elif operation == 'fraction': result = 0.0 if left==0 else left - math.floor(left)
        elif operation == 'nonzero': result = float(left!=0)
        elif operation == 'select_gt0': result = operands[1] if left > 0 else operands[2]
        elif operation == 'minimum': result = left if left < operands[1] else operands[1]
        else: result = left if left >= operands[1] else operands[1]
        result = struct.unpack('>f', struct.pack('>f', result))[0]
    except (OverflowError, ValueError, struct.error) as error:
        raise RawShaderError('loading result outside finite float32 policy') from error
    if not math.isfinite(result) or (result != 0 and abs(result) < 2**-126):
        raise RawShaderError('loading result nonfinite/subnormal unsupported')
    # Binary32 rounding to zero is included. Nonzero subnormal results above
    # still reject; this is not a claim about Xbox transcendental precision.
    return result
