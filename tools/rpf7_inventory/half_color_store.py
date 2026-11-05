"""Bounded half-color export to owned reference EDRAM; no guest mutation."""
from functools import lru_cache
import struct
from color_7e3 import bits,pack
from edram_alias_clear import EDRAM_BYTES,footprint
INPUT_BYTES=1280*720*8+32
OUTPUT_BYTES=EDRAM_BYTES+32
VIEW=(0x14000500,0xC02D0,1280,720,False)


def packed_half(rgba):
    # Validate before caching: bool-valued tuples compare equal to int tuples.
    if type(rgba) is not tuple or len(rgba)!=4 or any(type(h) is not int or not 0<=h<=65535 for h in rgba):
        raise ValueError('four unsigned half words required')
    return _packed_half(rgba)


@lru_cache(maxsize=65536)
def _packed_half(rgba):
    if any((h&0x7C00)==0x7C00 or ((h&0x7C00)==0 and h&1023) for h in rgba):return None
    channels=struct.unpack('<4e',struct.pack('<4H',*rgba))
    try:return pack(tuple(bits(c) for c in channels))
    except ValueError:return None


def store_reference(source):
    if not isinstance(source,bytes) or len(source)!=INPUT_BYTES:raise ValueError('exact immutable guarded half image required')
    guard=bytes([0x5A])*16
    if source[:16]!=guard or source[-16:]!=guard:raise ValueError('half source guard corrupted')
    layout=footprint(*VIEW);result=bytearray([0x5A])*OUTPUT_BYTES;written=0
    # Tile-major CPU traversal is independent of the shader's pixel-major map.
    for tile_y in range(45):
        for tile_x in range(16):
            for local_y in range(16):
                y=tile_y*16+local_y
                at=16+(y*1280+tile_x*80)*8
                words=struct.iter_unpack('<4H',source[at:at+80*8])
                target=16+layout['begin']+(tile_y*16+tile_x)*5120+local_y*80*4
                for local_x,rgba in enumerate(words):
                    packed=packed_half(rgba)
                    if packed is not None:struct.pack_into('<I',result,target+local_x*4,packed);written+=1
    return bytes(result),dict(source_bytes=len(source),output_bytes=len(result),written_pixels=written,
        rejected_pixels=1280*720-written,full_surface_written=written==1280*720,
        source_half_policy='finite-normal-or-signed-zero; subnormal-and-nonfinite-pixels-unwritten',
        packed_policy='checked-7e3-nearest-even-rgb-and-float32-alpha-multiply3-add-half-truncate',
        reference_footprint=layout,retail_format12_verified=False,guest_writeback=False)
