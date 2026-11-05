"""Unexecuted event request fields, not timestamps or completed GPU events.

Reference: Xenia95a5c3ee250f80c3b9d139658649d9ffb6db3eec xenos.h Event and
command_processor.cc EVENT_WRITE / EVENT_WRITE_SHD. Its one-word EVENT_WRITE
implementation does not implement native resource-cache flush obligations.
"""


def describe_event_write(opcode,payload):
    if (type(opcode) is not int or opcode not in (0x46,0x58) or
        type(payload) not in (tuple,list) or len(payload)!=(1 if opcode==0x46 else 3) or
        any(type(v) is not int or not 0<=v<=0xFFFFFFFF for v in payload)):
        raise ValueError('canonical bounded event payload required')
    initiator=payload[0]
    event=initiator&63
    result=dict(scope='unexecuted-event-request-reference',opcode=opcode,initiator=initiator,
        event_id=event,reference_event_name={3:'PS_DONE_TS',6:'CACHE_FLUSH'}.get(event),
        runtime_supported_in_draw_batch=False,event_executed=False,
        event_initiator_register=0x21F9,register_side_effect_executed=False,
        guest_memory_written=False,literal_write_candidate=None)
    if opcode==0x46:
        result.update(explicit_memory_destination=False,exact_cache_flush_request=initiator==6,
            native_resource_flush_required=initiator==6,cache_flush_completed=False,
            no_memory_payload_proves_no_side_effect=False)
    else:
        address=payload[1]&~3
        mode=payload[1]&3
        counter=bool(initiator&0x80000000)
        canonical=address+4<=0x20000000
        exact=initiator==3 and mode==2 and canonical
        result.update(explicit_memory_destination=True,physical_address=address,
            byte_count=4,endianness_mode=mode,canonical_physical_range=canonical,
            value_source='reference-internal-counter' if counter else 'packet-literal',
            packet_value=payload[2],counter_value_known=False,
            wall_clock_timestamp_verified=False,exact_literal_ps_done_be_profile=exact)
        if exact:
            result['literal_write_candidate']=dict(physical_address=address,byte_count=4,
                value=payload[2],big_endian_bytes_hex=payload[2].to_bytes(4,'big').hex(),
                allocation_ownership_verified=False,gpu_completion_verified=False)
    return result
