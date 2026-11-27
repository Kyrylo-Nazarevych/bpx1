#!/usr/bin/env python3
"""Checks what the control port read while requests were running (FR-10 during a live run, spec VR-01).

  conc_check.py <steps.ops> <background.log>

`cosim --rand` logs every background job with the window of requests [lo, hi] each word may reflect: lo = requests
acknowledged when the chip fetched the word, less two (a resolve is trained shortly after its acknowledge);
hi = requests started by then.  This script replays the steps through the golden model and requires, for every
word, that it equals the model's value after some whole number n of requests, lo <= n <= hi:
  statistics   both counters (resolved, wrong answers) from the same n (the snapshot is consistent), as running
               totals over all snapshot-and-clear steps: no event is lost or counted twice; 0x09-0x0E read 0
  pin counts   the per-predictor mispredictions the harness counted from the prediction pins over the whole run
               (line H of the log) equal the model's totals (mosaic step 5: these counts are the host's)
  raw reads    the entry as it is after n requests: never a half-written or stale value (space 12, the leaky
               twin, included: FCTL, and with it the read mask TWIN_EN, changes only between foreground steps)
  ERRCNT and the error flags lie between their values at lo and at hi; ID and CTRL are exact
The model keeps its fault state (FSTAT, the demotion count, DEMOTED) across requests: demotion changes the driving
prediction and with it the speculative history that raw reads of space 7 see.
"""
import sys

sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "model"))
import bpx1  # noqa: E402


def main():
    m = bpx1.Bpx1()
    cum = [0, 0]                   # unsaturated totals of the model: resolved, wrong answers (the chip's two counters)
    errs, flags = [0], [0]
    events = []                    # dicts: lo, hi, kind, test, ok
    acc = [0, 0]                   # totals of the chip: sum of the snapshot-and-clear results
    # the host's per-predictor counts from the pins (cosim.cpp, line H at the end of the log): resolved, wrong answers,
    # default, bimodal, gshare, tournament, default and tournament over the branches whose predict showed them on DBG
    pins = None
    mod = [0] * 8                  # the model's unsaturated totals of the same eight
    counts = dict(S=0, C=0, D=0, I=0, W=0, H=0, words=0)
    for ln in open(sys.argv[2]):
        t = ln.split()
        k = t[0]
        counts[k] += 1
        if k == "H":
            pins = list(map(int, t[1:10]))
        elif k in "SC":
            lo, hi, lo2, hi2 = map(int, t[1:5])
            w = list(map(int, t[5:17]))
            exp = [acc[i] + (w[2 + 2 * i] | (w[3 + 2 * i] << 16)) for i in range(2)]
            if k == "C":
                acc = exp
            # 0x09-0x0E read 0 since step 5 (the per-predictor counts are the FPGA's): otherwise the check fails
            events.append(dict(lo=lo, hi=hi, kind="stat", exp=None if any(w[6:12]) else exp, desc=ln.strip()))
            events.append(dict(lo=lo2, hi=hi2, kind="status", w0=w[0], w1=w[1], desc=ln.strip()))
        elif k == "D":
            sp, a, cnt = map(int, t[1:4])
            for i in range(cnt):
                lo, hi, v = map(int, t[4 + 3 * i:7 + 3 * i])
                events.append(dict(lo=lo, hi=hi, kind="raw", sp=sp, a=a + i, v=v, desc=f"raw space {sp} entry {a + i} = {v}"))
                counts["words"] += 1
        elif k == "I":
            lo, hi, w0, w1 = map(int, t[1:5])
            events.append(dict(lo=lo, hi=hi, kind="id", w0=w0, w1=w1, desc=ln.strip()))
    events.sort(key=lambda e: e["lo"])
    for e in events:
        e["ok"] = False
        e["seen"] = []
    n, nxt, active, bad, shown = 0, 0, [], 0, 0
    raw_addr = 0

    def evaluate():
        nonlocal nxt, active, bad, shown
        while nxt < len(events) and events[nxt]["lo"] <= n:
            active.append(events[nxt])
            nxt += 1
        keep = []
        for e in active:
            if n > e["hi"]:
                ok = e["ok"]
                if e["kind"] == "status":
                    s = e["seen"]
                    f = (e["w0"] >> 8) & 3
                    ok = bool(s) and min(255, s[0][0]) <= (e["w1"] & 0xff) <= min(255, s[-1][0]) and \
                        (s[0][1] & ~f) == 0 and (f & ~s[-1][1]) == 0 and not (e["w0"] >> 11) & 1
                if not ok:
                    bad += 1
                    if shown < 5:
                        shown += 1
                        print(f"MISMATCH window [{e['lo']}, {e['hi']}]: {e['desc']}; model: {e['seen'][:6]}")
                continue
            keep.append(e)
            if e["kind"] == "stat":
                e["ok"] = e["ok"] or e["exp"] == cum
                e["seen"].append(list(cum))
            elif e["kind"] == "raw":
                v = m.raw(e["sp"], e["a"])
                e["ok"] = e["ok"] or v == e["v"]
                e["seen"].append(v)
            elif e["kind"] == "id":
                e["ok"] = e["ok"] or (e["w0"] == 0xB101 and e["w1"] == m.ctrl_word())
                e["seen"].append(m.ctrl_word())
            else:
                e["seen"].append((errs[0], flags[0]))
        active = keep

    evaluate()
    for ln in open(sys.argv[1]):
        t = ln.split()
        if t[0] == "R":
            m.request(int(t[1]), int(t[2]))
            cum[0] += m.n_res
            cum[1] += m.mis_drv
            for i, v in enumerate([m.n_res, m.mis_drv] + m.mis + m.mis_shown):
                mod[i] += v
            errs[0] += m.n_err
            flags[0] |= m.err_flags & 3
            m.clear_counters()                           # not clear_stats(): FSTAT and demotion carry on

            n += 1
        elif t[0] == "W" and t[1] == "1":
            m.write_ctrl(int(t[2]))
        elif t[0] == "W" and t[1] == "17":                # FCTL (the twin's raw reads depend on TWIN_EN)
            m.write_fctl(int(t[2]))
        elif t[0] == "W" and t[1] == "15":
            raw_addr = int(t[2])
        elif t[0] == "W" and t[1] == "16":
            m.raw_write((raw_addr >> 8) & 15, raw_addr & 127, int(t[2]), (raw_addr >> 12) & 3)
        elif t[0] == "W" and t[1] == "2":
            if int(t[2]) != 2:
                sys.exit("conc_check: only 'clear predictors' is supported among the commands")
            m.clear_predictors()
        elif t[0] == "X" and t[1] == "W":
            m.raw_write(0, int(t[2]), int(t[3]))
        else:
            continue
        evaluate()
    n += 1 << 40
    evaluate()
    # per-predictor counts the harness made from the pins against the model: always resolved, wrong answers, bimodal,
    # gshare and the shown default and tournament; the complete default and tournament if DBG showed every branch
    if pins is None:
        pin_ok = False
        print("MISMATCH: no host pin counts (line H) in the background log")
    else:
        idx = [0, 1, 3, 4, 6, 7] + ([2, 5] if pins[8] == 0 else [])
        pin_ok = all(pins[i] == mod[i] for i in idx) and (pins[8] == 1 or (mod[2] == mod[6] and mod[5] == mod[7]))
        if not pin_ok:
            print(f"MISMATCH host pin counts {pins[:8]} (DBG not always standard: {pins[8]}), model {mod}")
    bad += 0 if pin_ok else 1
    print(f"background: snapshots={counts['S']} snapshot_clears={counts['C']} raw_reads={counts['D']} raw_words={counts['words']} "
          f"raw_writes={counts['W']} id_reads={counts['I']} checks={len(events) + 1} mismatches={bad} totals={cum} "
          f"pin_counts={'ok' if pin_ok else 'MISMATCH'} res/drv/d/b/g/t/ds/ts={mod}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
