"""CPU reference for the explicit 640x360 4x native diagnostic, not title state."""
from array import array
import math
import struct
import sys
from edram_alias_clear import EDRAM_BYTES, pack_canonical_depth
from rectangle_vertex_stage import evaluate_rectangle
from half_color_load import unpack_half

BACKING_BYTES = EDRAM_BYTES + 32
COLOR_BYTES = 1280*720*4 + 32
EXPECTED_BYTES = BACKING_BYTES + COLOR_BYTES
CLEAR_WORD = 0x200000A5


def uniform_depth_initial_pixel(code, source):
    """Narrow full-rectangle/all-four-samples oracle for native chain initialization."""
    proof = evaluate_rectangle(code, source, 640, 360)
    if proof['source_bounds'] != [-.5, -.5, 639.5, 359.5]:
        raise ValueError('exact full-view depth rectangle required for uniform initialization')
    value = proof['source_exports'][0]['62'][2]
    bits = struct.unpack('<I', struct.pack('<f', value))[0]
    word = pack_canonical_depth(bits, 0)  # Reject noncanonical and negative-zero depth.
    return unpack_half(word), dict(proof, initial_packed_word=word, depth_bits=bits,
        native_sample_mask=15, native_samples=4, native_depth_extent=[640, 360],
        canonical_clear_packing_only=True, uniform_initial_reference=True,
        cpu_supplied_initial_edram=False, live_depth_connected=True,
        gpu_executed=False, retail_sample_locations_verified=False)


def reference_words(bounds, depth_bits, sample_mask):
    """Inverse color address -> depth pixel/sample, independent of GLSL store."""
    if type(sample_mask) is not int or not 0 <= sample_mask <= 15:
        raise ValueError('four-bit explicit native sample mask required')
    if len(bounds) != 4 or any(not math.isfinite(v) or abs(v) > 16384 or (v+.5)%1 for v in bounds):
        raise ValueError('whole native pixel edges required, no subpixel reference claim')
    left, top, right, bottom = [v+.5 for v in bounds]
    if left >= right or top >= bottom or type(depth_bits) is not int or not 0 <= depth_bits <= 0x3F800000:
        raise ValueError('positive rectangle and positive float depth in [0,1] required')
    try:
        written = pack_canonical_depth(depth_bits, 0)
    except ValueError:
        written = 0x5A5A5A5A  # Native canonical-packing guard leaves this sample unwritten.
    rows = []
    for parity in range(2):
        row = []
        for color_x in range(1280):
            sample_x = (color_x//80)*80 + ((color_x%80+40)%80)
            pixel_x, horizontal_sample = divmod(sample_x, 2)
            sample = horizontal_sample*2 + parity
            row.append(written if left <= pixel_x < right and sample_mask & (1 << sample) else CLEAR_WORD)
        rows.append(array('I', row))
    clear_row = array('I', [CLEAR_WORD])*1280
    color = array('I')
    for y in range(720):
        color.extend(rows[y%2] if top <= y//2 < bottom else clear_row)
    if sys.byteorder != 'little':
        color.byteswap()
    plane = color.tobytes()
    backing = bytearray([0x5A])*BACKING_BYTES
    # Color-plane rows -> physical tile rows. No calls to native or GLSL layout.
    for tile_y in range(45):
        for tile_x in range(16):
            tile = 720 + tile_y*16 + tile_x
            for row in range(16):
                begin = ((tile_y*16+row)*1280+tile_x*80)*4
                at = 16 + tile*5120 + row*320
                backing[at:at+320] = plane[begin:begin+320]
    return bytes(backing)+bytes([0x5A])*16+plane+bytes([0x5A])*16


def make_alias_reference(code, source, sample_mask):
    proof = evaluate_rectangle(code, source, 640, 360)
    value = proof['source_exports'][0]['62'][2]
    if value == 0 and math.copysign(1, value) < 0:
        raise ValueError('negative-zero raster depth is outside the bit-exact reference')
    bits = struct.unpack('<I', struct.pack('<f', value))[0]
    return reference_words(proof['source_bounds'], bits, sample_mask), dict(proof,
        native_sample_mask=sample_mask, native_depth_extent=[640,360], native_samples=4,
        native_depth_format='D32_SFLOAT_S8_UINT', native_to_reference_sample_index='identity-test-policy',
        reference_storage_bytes=EDRAM_BYTES, color_word_extent=[1280,720],
        expected_bytes=EXPECTED_BYTES, canonical_clear_packing_only=True,
        numeric_color_conversion=False, retail_sample_locations_verified=False,
        resolve_executed=False, title_draw_executed=False, guest_writeback=False)
