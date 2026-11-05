"""Read-only coherency WAIT dependencies, not satisfied waits or a PM4 backend.

Reference: Xenia95a5c3ee250f80c3b9d139658649d9ffb6db3eec, registers.h,
register_table.inc and command_processor.cc WriteRegister/MakeCoherent/WAIT_REG_MEM.
That MakeCoherent implementation has an unimplemented cache-notification TODO;
copying its status clear is not proof of resource coherence in this renderer.
"""
from register_knowledge import RegisterKnowledge

REGISTERS=(0xA2F,0xA30,0xA31,*range(0x2006,0x200E),0x2319)


def describe_coherency_wait(payload,candidates):
    if (type(payload) not in (list,tuple) or len(payload)!=5 or
        any(type(v) is not int or not 0<=v<=0xFFFFFFFF for v in payload)):
        raise ValueError('five unsigned WAIT words required')
    if type(candidates) is not dict or set(candidates)!=set(REGISTERS):
        raise ValueError('complete explicit register candidate set required')
    if any(not isinstance(v,RegisterKnowledge) for v in candidates.values()):
        raise ValueError('validated bitwise register knowledge required')
    info,address,reference,mask,interval=payload
    exact=(info,address,reference,mask,interval)==(3,0xA31,0,0x80000000,8)
    status=candidates[0xA31];size=candidates[0xA2F];base=candidates[0xA30]
    def enabled(bit):
        return bool(status.value&(1<<bit)) if status.known_mask&(1<<bit) else None
    selected=[]
    for bit,register,name in [(8,0x2319,'copy-destination')]+[(9+i,0x2006+i,f'destination-{i}') for i in range(8)]:
        selected.append(dict(name=name,enable_bit=bit,enabled_candidate=enabled(bit),
            register=register,base_candidate=candidates[register].metadata()))
    complete=all(v.known_mask==0xFFFFFFFF for v in (status,size,base))
    empty_profile=None
    if complete:
        empty_profile=(status.value in (0x01000000,0x03000000) and size.value>0 and
            not ((base.value|size.value)&0xFFF) and base.value+size.value<=0x20000000)
    return dict(scope='command-order-coherency-dependencies-only',
        exact_observed_busy_wait_shape=exact,request=dict(info=info,address=address,
            reference=reference,mask=mask,interval=interval),
        status_candidate=status.metadata(),size_candidate=size.metadata(),base_candidate=base.metadata(),
        selected_destination_candidates=selected,
        # Reference WriteRegister forces busy independently of the literal's
        # bit31. This is deliberately separate from syntactic candidate state.
        xenia_status_after_write_reference=status.read_modify_write(0xFFFFFFFF,0x80000000).metadata(),
        reference_assumes_status_write_executed=True,
        existing_empty_cache_range_profile_compatible=empty_profile,
        zero_size_proves_no_work=False,cache_action_completed=False,
        runtime_supported=False,wait_result=None,guest_wait_executed=False)
