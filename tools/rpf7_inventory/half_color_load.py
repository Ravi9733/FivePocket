"""Packed reference EDRAM -> half-color image; no guest initialization claim."""
from functools import lru_cache
import struct
from color_7e3 import decode,f32
from edram_alias_clear import EDRAM_BYTES,footprint
INPUT_BYTES=EDRAM_BYTES+32
OUTPUT_BYTES=1280*720*8+32
VIEW=(0x14000500,0xC02D0,1280,720,False)


def unpack_half(packed):
    if type(packed) is not int or not 0<=packed<=0xFFFFFFFF:raise ValueError('unsigned packed color required')
    return _unpack_half(packed)


@lru_cache(maxsize=65536)
def _unpack_half(packed):
    channels=[decode((packed>>(i*10))&1023) for i in range(3)]+[f32((packed>>30)*f32(1/3))]
    result=struct.pack('<4e',*channels)
    if any(struct.unpack('<4e',result)[i]!=channels[i] for i in range(3)):
        raise ValueError('RGB must be exactly representable in binary16')
    return result


def load_reference(source):
    if not isinstance(source,bytes) or len(source)!=INPUT_BYTES:raise ValueError('exact immutable guarded EDRAM required')
    guard=bytes([0x5A])*16
    if source[:16]!=guard or source[-16:]!=guard:raise ValueError('EDRAM source guard corrupted')
    layout=footprint(*VIEW);result=bytearray([0x5A])*OUTPUT_BYTES
    for tile_y in range(45):
        for tile_x in range(16):
            for local_y in range(16):
                source_at=16+layout['begin']+(tile_y*16+tile_x)*5120+local_y*80*4
                to=16+((tile_y*16+local_y)*1280+tile_x*80)*8
                for local_x,(packed,) in enumerate(struct.iter_unpack('<I',source[source_at:source_at+320])):
                    result[to+local_x*8:to+(local_x+1)*8]=unpack_half(packed)
    return bytes(result),dict(input_bytes=len(source),output_bytes=len(result),decoded_pixels=921600,
        rgb_half_policy='exact',alpha_half_policy='nearest-even-and-toward-zero-agree-for-four-levels',
        initialized_guest_contents_verified=False,guest_writeback=False)
