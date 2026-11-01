#!/usr/bin/env python3
"""Compare the translated game with the original executable, frame by frame.

Both run the same input: Play is pressed, a test pilot taps through the pipes, the bird is
then dropped, and the game is restarted, for several seeds. After every frame the bird,
pipes, score, random-number state, fades, shake, queued events and the complete list of
drawn quads (image, four corners, alpha) must be identical.
"""
import ctypes
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from original_oracle import Original, APP, f32


class Quad(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int), ('q', ctypes.c_float * 8), ('alpha', ctypes.c_float)]


def images():
    out = []
    for line in (APP / 'atlas.txt').read_text().splitlines():
        p = line.split()
        if len(p) == 7:
            out.append((p[0], int(p[1]), int(p[2]), f32(float(p[3])), f32(float(p[4]))))
    return out


def run(seed, rounds, give_up_after, frames_limit=20000):
    lib = ctypes.CDLL(str(ROOT / 'build/probe.dylib'))
    lib.probe_add_image.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.probe_start.argtypes = [ctypes.c_uint, ctypes.c_float]
    atlas = images()
    lib.probe_reset()
    for name, w, h, _, _ in atlas:
        lib.probe_add_image(name.encode(), w, h)
    by_uv = {(u, v): i for i, (_, _, _, u, v) in enumerate(atlas)}
    names = [a[0] for a in atlas]
    assert lib.probe_start(seed, ctypes.c_float(240.0)) == 0
    vm = Original(seed, screen_width=240.0)

    floats, ints = (ctypes.c_float * 7)(), (ctypes.c_int * 24)()
    quads, events = (Quad * 512)(), (ctypes.c_int * 128)()
    frame = quad_total = 0
    best_score = flaps = 0
    hold = 0                    # frames left of a held touch
    touch = (0, 0)
    tap_down = False
    for round_index in range(rounds):
        press_play_at = frame + 40
        round_start = frame
        state = 'menu'
        while True:
            # ---- input for this frame, decided from the port's state before it runs ----
            lib.probe_state(floats, ints)
            score, speed, title = ints[6], ints[9], ints[20]
            down = False
            if state == 'menu':
                if frame == press_play_at:
                    hold, touch = 4, (78, 375)           # the Play button
                if hold:
                    down = True
                    hold -= 1
                    if not hold:
                        state = 'play'
            elif state == 'play' and not title:
                want = lib.probe_pilot() and score < give_up_after
                tap_down = bool(want) and not tap_down
                down, touch = tap_down, (144, 300)
                flaps += down
                if speed == 0 and frame - round_start > 120:
                    state = 'over'
                    over_at = frame
            elif state == 'over':
                if frame - over_at > 260:               # panel has counted up; buttons are back
                    break
            vm.frame(down, *touch)
            lib.probe_frame(int(down), *touch)
            frame += 1

            # ---- compare ----
            of, oi = vm.state()
            lib.probe_state(floats, ints)
            where = 'seed %d frame %d' % (seed, frame)
            assert tuple(floats) == tuple(f32(x) for x in of), (where, 'floats', tuple(floats), of)
            assert list(ints) == oi, (where, 'ints', list(ints), oi)
            n = lib.probe_events(events)
            mine = [(events[2 * i], events[2 * i + 1]) for i in range(n)]
            assert mine == vm.events(), (where, 'events', mine, vm.events())
            n = lib.probe_quads(quads)
            theirs = vm.scene
            assert n == len(theirs), (where, 'quad count', n, len(theirs))
            for k in range(n):
                uv, corners, alpha = theirs[k]
                image = by_uv[uv]
                expected = -1 if names[image] == 'black' else -2 if names[image] == 'white' else image
                q = quads[k]
                assert q.id == expected, (where, 'quad', k, names[image], q.id)
                assert tuple(q.q) == tuple(float(c) for c in corners), (where, 'quad', k, names[image], tuple(q.q), corners)
                assert q.alpha == alpha, (where, 'alpha', k, names[image], q.alpha, alpha)
            quad_total += n
            best_score = max(best_score, score)
            assert frame < frames_limit, 'run did not finish'
    return frame, quad_total, best_score, flaps


if __name__ == '__main__':
    for seed, rounds, give_up in ((12345, 3, 3), (20140209, 2, 12), (777, 2, 45)):
        frames, quad_count, best, flaps = run(seed, rounds, give_up)
        print('PASS: seed %d: %d frames, %d quads, %d taps, best score %d, %d rounds identical to the original'
              % (seed, frames, quad_count, flaps, best, rounds), flush=True)
