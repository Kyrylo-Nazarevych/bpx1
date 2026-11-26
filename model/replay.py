#!/usr/bin/env python3
"""Replay a request log of the co-simulation (cosim/cosim.cpp) through the golden model (spec VR-01).

  replay.py <oplog.bin> <drive>

Every reply of the RTL (prediction and confidence) is compared with the model; prints the model's statistics in the
same key=value form as the co-simulation, and mismatches=<n> with the first mismatching request.
"""
import sys
import bpx1


def main():
    data = open(sys.argv[1], "rb").read()
    m = bpx1.Bpx1(bpx1.Config(STATW=40))
    m.drive = int(sys.argv[2])
    bad, first = 0, None
    for i in range(0, len(data), 4):
        op, ident, rep = data[i], data[i + 1] | (data[i + 2] << 8), data[i + 3]
        r = m.request(op, ident)
        if op != bpx1.OP_SPECIAL and (r.pred | (r.conf << 1)) != rep:
            bad += 1
            if first is None:
                first = (i // 4, op, ident, rep, r.pred | (r.conf << 1))
    s = m.stats()
    print(f"requests={len(data) // 4} mismatches={bad} first={first} errors={s['errors']} resolved={s['resolved']} mis_def={s['mis_default']} "
          f"mis_bim={s['mis_bimodal']} mis_gsh={s['mis_gshare']} mis_trn={s['mis_tournament']}")


if __name__ == "__main__":
    main()
