#!/usr/bin/env python3
"""Print 16 bytes (as hex and as 4 big-endian floats) at addresses of a relocated PRX.

usage: peek.py module.prx ADDR [ADDR...]   (addresses in hex)
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prxdis import Prx

p = Prx(sys.argv[1])
for a in sys.argv[2:]:
    a = int(a, 16)
    o = p.v2o(a)
    b = bytes(p.d[o:o + 16])
    print(hex(a), b.hex(' ', 4), [round(x, 6) for x in struct.unpack('>4f', b)])
