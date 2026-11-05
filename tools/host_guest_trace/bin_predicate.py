"""Conservative (mask & select) reference, not a retail reset-state oracle.

Reference: Xenia95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
src/xenia/gpu/command_processor.cc: ExecutePacketType3. Only draw bin predicates
are modeled here; query conditions, swap special cases and retirement are not.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class BinState:
    # mask low/high, select low/high. None means unknown, never an assumed zero.
    words: tuple = (None, None, None, None)

    def __post_init__(self):
        if (type(self.words) is not tuple or len(self.words)!=4 or
                any(v is not None and (type(v) is not int or not 0<=v<=0xFFFFFFFF) for v in self.words)):
            raise ValueError('four optional unsigned bin words required')

    def assign(self,index,value):
        if type(index) is not int or not 0<=index<4 or type(value) is not int or not 0<=value<=0xFFFFFFFF:
            raise ValueError('bounded bin assignment required')
        words=list(self.words);words[index]=value
        return BinState(tuple(words))

    def reference_result(self,predicated):
        if type(predicated) is not bool:raise ValueError('explicit predicate bit required')
        if not predicated:return True
        uncertain=False
        for mask,select in zip(self.words[:2],self.words[2:]):
            if mask is not None and select is not None:
                if mask&select:return True
            elif mask!=0 and select!=0:uncertain=True
        return None if uncertain else False

    def metadata(self,predicated):
        return {'word_order':'mask-low,mask-high,select-low,select-high',
            'words':list(self.words),'known_words':sum(1<<i for i,w in enumerate(self.words) if w is not None),
            'reference_result':self.reference_result(predicated),
            'policy':'xenia-mask-and-select-nonzero','retail_reset_state_verified':False,
            'guest_predicate_executed':False}


def retained_bins(record):
    if record is None:return BinState()
    if (type(record) is not dict or record.get('profile')!='retained-command-written-bin-words' or
        record.get('word_order')!='low-high' or record.get('unknown_words_are_reset_values') is not False or
        record.get('predicates_executed') is not False or record.get('retail_reset_state_verified') is not False):
        raise ValueError('explicit retained native bin evidence required')
    groups=[record.get(k) for k in ('mask_words','select_words')]
    if any(type(g) is not list or len(g)!=2 for g in groups):raise ValueError('two low/high bin-word pairs required')
    words=groups[0]+groups[1];known=record.get('known_words')
    if (type(known) is not int or not 0<=known<=15 or
        any(type(w) is not int or not 0<=w<=0xFFFFFFFF or (w and not known&(1<<i)) for i,w in enumerate(words))):
        raise ValueError('bounded known-word mask without fabricated unknown values required')
    return BinState(tuple(w if known&(1<<i) else None for i,w in enumerate(words)))
