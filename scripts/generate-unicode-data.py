"""Generate RFC 3454/4013 Unicode 3.2 property ranges, using Python's frozen database."""
from pathlib import Path
import stringprep

tables={
    'sasl_nothing': stringprep.in_table_b1,
    'sasl_space': stringprep.in_table_c12,
    'sasl_unassigned': stringprep.in_table_a1,
    'sasl_prohibited': lambda c: any(getattr(stringprep,'in_table_'+t)(c) for t in ('c12','c21','c22','c3','c4','c5','c6','c7','c8','c9')),
    'sasl_randal': stringprep.in_table_d1,
    'sasl_lcat': stringprep.in_table_d2,
}
output=['// Generated Unicode 3.2 property ranges for RFC 3454 and RFC 4013.\n']
for name,predicate in tables.items():
    ranges=[]
    for cp in range(0x110000):
        if predicate(chr(cp)):
            if ranges and ranges[-1][1]+1==cp: ranges[-1][1]=cp
            else: ranges.append([cp,cp])
    output.append('static constexpr UnicodeRange '+name+'[]={\n')
    output.extend(f'    {{0x{a:x},0x{b:x}}},\n' for a,b in ranges)
    output.append('};\n')
Path(__file__).resolve().parents[1].joinpath('src/unicode_ranges.inc').write_text(''.join(output),encoding='utf-8')
