"""Extract a finite-input XY position contract from proven vertex dataflow.

This is shader metadata, not evidence of draw coverage: native execution must
evaluate the actual vertices/constants, viewport and triangle connectivity.
No captured payloads or particular constant indices are embedded here.
"""


def affine_position_xy(proof):
    nodes, fetches = proof['nodes'], proof['fetches']

    def node(index):
        seen = set()
        while type(index) is int and 0 <= index < len(nodes) and index not in seen:
            seen.add(index)
            value = nodes[index]
            if value[0] != 'regular_copy':
                return value
            index = value[1]
        raise ValueError('invalid or cyclic position dataflow')

    def constant(value):
        if (len(value) != 3 or value[0] != 'constant' or
                not 0 <= value[1] < 256 or not 0 <= value[2] < 4):
            raise ValueError('direct float constant required')
        return value[1] * 4 + value[2]

    def axis(index):
        value = node(index)
        if value[0] != 'add':
            raise ValueError('affine addition required')
        terms = [node(i) for i in value[1:]]
        if terms[0][0] == 'constant':
            terms.reverse()
        product, bias = terms
        if product[0] != 'mul':
            raise ValueError('affine multiply required')
        factors = [node(i) for i in product[1:]]
        if factors[0][0] == 'constant':
            factors.reverse()
        source, scale = factors
        if source[0] != 'fetch' or not 0 <= source[1] < len(fetches):
            raise ValueError('direct fetched component required')
        fetch = fetches[source[1]]
        if (fetch['fetch_slot'] != 95 or fetch['format'] not in (37, 38, 57) or
                node(fetch['index_expression']) != ('initial', 0, 0) or
                not 0 <= source[2] < fetch['components']):
            raise ValueError('inline float stream required')
        offset = fetch['offset_words'] + source[2]
        if not 0 <= offset < fetch['stride_words']:
            raise ValueError('position outside vertex stride')
        return offset * 4, constant(scale), constant(bias), fetch['stride_words'] * 4

    try:
        position = proof['exports']['62']
        if len(position) != 4 or node(position[2]) != ('literal', 0) or node(position[3]) != ('literal', 1):
            return None
        axes = [axis(i) for i in position[:2]]
        if axes[0][3] != axes[1][3]:
            return None
        return dict(offset_bytes=[a[0] for a in axes], scale_words=[a[1] for a in axes],
                    bias_words=[a[2] for a in axes], stride=axes[0][3])
    except (ValueError, KeyError, IndexError, TypeError):
        return None
