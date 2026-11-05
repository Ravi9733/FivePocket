"""Bitwise knowledge for stopped register analysis, never a reset-value model."""
from dataclasses import dataclass

WORD_MASK = 0xFFFFFFFF


def _word(value):
    if type(value) is not int or not 0 <= value <= WORD_MASK:
        raise ValueError('register value is not an unsigned word')
    return value


@dataclass(frozen=True)
class RegisterKnowledge:
    value: int = 0
    known_mask: int = 0

    def __post_init__(self):
        _word(self.value)
        _word(self.known_mask)
        if self.value & ~self.known_mask:
            raise ValueError('unknown register bits must not carry a claimed value')

    @classmethod
    def assigned(cls, value):
        return cls(_word(value), WORD_MASK)

    def read_modify_write(self, and_mask, or_mask):
        and_mask, or_mask = _word(and_mask), _word(or_mask)
        # AND-zero and OR-one make bits known even when input was unknown.
        known = self.known_mask | (WORD_MASK ^ and_mask) | or_mask
        return RegisterKnowledge(((self.value & and_mask) | or_mask) & known, known)

    def require(self, mask=WORD_MASK):
        mask = _word(mask)
        if mask & ~self.known_mask:
            raise ValueError('required register bits depend on unknown initial state')
        return self.value & mask

    def metadata(self):
        return {'value_bits': self.value, 'known_mask': self.known_mask,
                'fully_known': self.known_mask == WORD_MASK,
                'hardware_side_effects_validated': False}
