#!/usr/bin/env python3
"""Verify and test a complete inline-table leaf that stores a scaled field."""
from pathlib import Path
import argparse
import os
import subprocess

from switch_tables import MAX_IMAGE_BYTES, discover_u32_switches, function_words, signed16
from leaf_switch_fixture import extract_function
from switch_target_fixture import checked_fragment


def d(op, target, base, immediate):
    return op << 26 | target << 21 | base << 16 | (immediate & 0xFFFF)


def x(target, left, right, operation):
    return 31 << 26 | target << 21 | left << 16 | right << 11 | operation << 1


def rotate(source, target, shift, first, last):
    return 21 << 26 | source << 21 | target << 16 | shift << 11 | first << 6 | last << 1


def conditional(bo, bi, displacement):
    return 16 << 26 | bo << 21 | bi << 16 | displacement


def product_tail(width_offset, output_offset):
    """Canonical compiler pattern: minimum4 for selectors1..8, else minimum1.

    A bounded unsigned halfword count and a positive <=128 multiplier make the
    signed multiply/divide-by8 sequence nonnegative and free of overflow.
    Encodings here describe a generic instruction idiom, not game addresses.
    """
    words = []
    for value in range(1, 8):
        words += [d(11, 24, 10, value), conditional(12, 26, (17-len(words)-1)*4)]
    words += [d(11, 24, 10, 8), d(14, 10, 0, 0), conditional(4, 26, 8), d(14, 10, 0, 1)]
    words += [rotate(10, 9, 0, 24, 31), d(40, 10, 3, width_offset), d(8, 8, 9, 0),
        x(7, 8, 8, 136), rotate(7, 9, 0, 30, 31), d(14, 9, 9, 1),
        x(24, 10, 9, 0), conditional(12, 25, 8), x(9, 10, 9, 444),
        x(11, 10, 11, 235), x(11, 10, 3, 824), x(9, 10, 0, 202),
        d(44, 9, 3, output_offset), 0x4E800020]
    return words


def verify_field_product_leaf(image, image_base, base, size):
    words = function_words(image, image_base, base, size)
    tables = discover_u32_switches(image, image_base, base, size)
    if len(words) < 45 or len(tables) != 1 or tables[0].base != base+16 or tables[0].selector != 10:
        raise ValueError('not one complete inline-table field-product leaf')
    table = tables[0]
    # General switch discovery permits many register choices. This whole-leaf
    # proof additionally requires the canonical volatile-register assignment;
    # otherwise the dispatch could overwrite r3, SP, or a nonvolatile register
    # before the independently verified shared tail consumes the object.
    shift = words[5] if words[5] >> 26 == 21 else words[6]
    if words[4] & 0xFFFF0000 != d(15,12,0,0) or shift != rotate(10,0,2,0,29) or \
            words[2] != d(10,24,10,len(table.labels)-1):
        raise ValueError('dispatch register assignment is outside the complete-leaf proof')
    selector_offset = signed16(words[0])
    if words[0] != d(32, 11, 3, selector_offset) or not 0 <= selector_offset <= 1024 or selector_offset % 4 \
            or words[1] != rotate(11, 10, 0, 3, 31):
        raise ValueError('unexpected selector load or mask')
    low = words[5] if words[5] >> 26 == 14 else words[6]
    table_address = (((words[4] & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
    table_end = table_address + 4*len(table.labels)
    tail = table.default + 4
    targets = sorted(set(table.labels) | {table.default})
    if table_address != base+40 or not 1 <= len(table.labels) <= 256 or \
            targets != [*range(table_end, table.default, 8), table.default] or \
            not 1 <= len(targets)-1 <= 32 or tail+128 != base+size:
        raise ValueError('table, cases and shared tail are not contiguous and exhaustive')
    multipliers = {}
    for target in targets[:-1]:
        index = (target-base)//4
        value = signed16(words[index])
        if words[index] != d(14, 11, 0, value) or value <= 0 or value > 128 or value % 4 \
                or words[index+1] != (18 << 26 | (tail-target-4)):
            raise ValueError('case is not a bounded multiplier followed by the shared tail')
        multipliers[target] = value
    start = (tail-base)//4
    width_offset, output_offset = signed16(words[start+19]), signed16(words[start+30])
    if words[start-1] != d(14, 11, 0, 0) or any(not 0 <= at <= 1024 or at % 2 for at in (width_offset,output_offset)) \
            or list(words[start:]) != product_tail(width_offset,output_offset):
        raise ValueError('shared arithmetic, memory effects or return differs from the verified pattern')
    # Keep independent input/output canaries unambiguous in the executable test.
    intervals = [(selector_offset,4),(width_offset,2),(output_offset,2)]
    if any(a < b+m and b < a+n for i,(a,n) in enumerate(intervals) for b,m in intervals[i+1:]):
        raise ValueError('leaf fields overlap')
    multipliers[table.default] = 0
    return {'table':table, 'selector_offset':selector_offset, 'width_offset':width_offset,
        'output_offset':output_offset, 'multipliers':tuple(multipliers[t] for t in table.labels),
        'table_address':table_address}


def fixture_source(function, verified):
    table = verified['table']
    checked_fragment(function, table)  # Actual dispatch retains live CTR validation.
    if 'ERROR ' in function or 'PPC_RUNTIME_TRAP' in function:
        raise ValueError('leaf still has unresolved code')
    text = '#include "ppc_context.h"\n#include "guest_address_space.h"\n#include <algorithm>\n#include <array>\n#include <cassert>\n#include <cstdio>\n'
    text += '#undef PPC_CALL_INDIRECT_FUNC\n#define PPC_CALL_INDIRECT_FUNC(target) do { assert(false); } while(false)\n'
    text += function + '\nint main() {\nrecomp::bootstrap::GuestAddressSpace space; assert(space.reserve()); auto* base=space.base();\n'
    text += 'const auto put=[&](uint32_t at,uint32_t value,unsigned size) {for(unsigned i=0;i<size;++i) base[at+i]=uint8_t(value>>((size-i-1)*8));};\n'
    text += f'const std::array<uint32_t,{len(table.labels)}> targets{{'+','.join(f'0x{x:X}U' for x in table.labels)+'};\n'
    text += f'const std::array<uint32_t,{len(table.labels)}> bits{{'+','.join(map(str,verified['multipliers']))+'};\n'
    text += f'for(unsigned i=0;i<targets.size();++i) put(0x{verified["table_address"]:X}U+i*4,targets[i],4);\n'
    text += 'uint64_t checks=0; for(uint32_t selector=0;selector<bits.size()+3;++selector) {\n'
    text += 'const uint32_t raw=selector<bits.size()+2?selector:0x1FFFFFFF;\n'
    text += 'for(uint32_t count=0;count<=65535;++count) {\n'
    text += 'for(uint32_t high : {0U,0xE0000000U}) {\nPPCContext ctx{}; ctx.r3.u64=0x1000; ctx.r1.u64=0x8800; ctx.lr=0xABCDEF0123456789ULL; ctx.r24.u64=0xFEDCBA9876543210ULL;\n'
    text += 'std::array<uint8_t,1040> before; before.fill(0xA5); std::copy(before.begin(),before.end(),base+0x1000);\n'
    text += f'put(0x{0x1000+verified["selector_offset"]:X},raw|high,4); put(0x{0x1000+verified["width_offset"]:X},count,2);\n'
    text += 'std::copy_n(base+0x1000,before.size(),before.begin());\n'
    text += f'__imp__sub_{table.base-16:X}(ctx,base);\n'
    text += 'const auto expected=uint16_t((raw<bits.size()?bits[raw]:0)*std::max(count,raw>=1&&raw<=8?4U:1U)/8U);\n'
    text += f'before[{verified["output_offset"]}]=uint8_t(expected>>8); before[{verified["output_offset"]+1}]=uint8_t(expected);\n'
    text += 'assert(std::equal(before.begin(),before.end(),base+0x1000));\n'
    text += 'assert(ctx.r3.u64==0x1000 && ctx.r1.u64==0x8800 && ctx.lr==0xABCDEF0123456789ULL && ctx.r24.u64==0xFEDCBA9876543210ULL); ++checks;\n'
    text += '}}} std::printf("FIELD_PRODUCT_LEAF_V1 full_body_checks=%llu canaries=1 frame_presented=0\\n",static_cast<unsigned long long>(checks));\n}\n'
    return text


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('source',type=Path)
    parser.add_argument('--image-base',type=lambda v:int(v,0),required=True)
    parser.add_argument('--function-base',type=lambda v:int(v,0),required=True)
    parser.add_argument('--function-size',type=lambda v:int(v,0),required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    for path in (args.image,args.source,args.output):
        lexical=path.absolute()
        if lexical.is_symlink() or lexical!=lexical.resolve() or not lexical.is_relative_to(root/'local-analysis') or \
                subprocess.run(['git','check-ignore','--quiet','--',str(lexical)],cwd=root).returncode:
            raise ValueError('fixture paths must be private ignored regular paths')
    if not 0<args.image.stat().st_size<=MAX_IMAGE_BYTES or not 0<args.source.stat().st_size<=8*1024*1024:
        raise ValueError('fixture input exceeds size bound')
    verified=verify_field_product_leaf(args.image.read_bytes(),args.image_base,args.function_base,args.function_size)
    function=extract_function(args.source.read_text(),args.function_base,allow_indirect=True)
    text=fixture_source(function,verified)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w', encoding='utf-8', newline='\n') as stream: stream.write(text)
    print('Prepared private complete-leaf fixture: every 16-bit count, all cases and invalid selectors, masked high bits')


if __name__=='__main__':
    try: main()
    except (ValueError,OSError): raise SystemExit('private field-product fixture validation failed')
