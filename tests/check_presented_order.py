"""Compare broker PRESENTED trace against independently demuxed PTS JSON.
Usage: python3 tests/check_presented_order.py expected-pts.json workload.log
Expected is an ordered JSON array of integer nanosecond PTS, not unique counts.
"""
import json
import re
import sys
PATTERN=re.compile(r'Video presented identity: serial=(\d+) fb=(\d+) pts=(\d+)')
def validate(expected, records):
    actual=[]
    previous_serial=0
    for serial, fb, pts in records:
        if serial <= previous_serial or not fb:
            raise ValueError('non-increasing SHOW serial or zero framebuffer')
        previous_serial=serial
        # Only the initial preroll frame may be shown again before progression.
        if len(actual)==1 and pts==actual[0]:
            continue
        actual.append(pts)
    if actual != expected:
        raise ValueError(f'ordered PTS mismatch: expected {len(expected)}, actual {len(actual)}')
    return len(actual)
def main():
    with open(sys.argv[1]) as stream: expected=json.load(stream)
    with open(sys.argv[2]) as stream: text=stream.read()
    loops=text.split('Video EOS drain:')
    count=0
    for loop in loops[:-1]:
        records=[tuple(map(int,m)) for m in PATTERN.findall(loop)]
        validate(expected,records); count+=1
    if not count: raise ValueError('no complete EOS trace')
    print(f'{count} EOS loops match the independent ordered PTS manifest')
if __name__=='__main__': main()
