from tools.ao_check.__main__ import misses


def line(flip, ran, ord_, want, at, fb, ok=1):
    return (
        f"[d3d11] ao: flip {flip} shown@8313c000 ran={ran} ord={ord_} want={want} "
        f"at={at} ok={ok} conf=1.000 ns=256 late=0 drop=0 fb={fb} ev=z10"
    )


def test_missed_frame_counts_after_warmup():
    lines = [line(i, 1, 4, 4, 4, 0) for i in range(1, 11)]
    lines.append(line(11, 0, 4, 6, 0, 0))
    steady, missed = misses(lines, warmup=8)
    assert steady == 3 and missed == [11]


def test_fallback_and_late_fire_are_not_misses():
    lines = [line(i, 1, 4, 4, 4, 0) for i in range(1, 10)]
    lines += [line(10, 1, 4, 5, 0, 1), line(11, 1, 6, 4, 4, 0)]
    assert misses(lines, warmup=8)[1] == []


def test_scene_start_restarts_warmup():
    lines = [line(i, 1, 4, 4, 4, 0) for i in range(1, 10)]
    lines.append(line(10, 0, 3, 0, 0, 0))
    lines += [line(11, 0, 2, 4, 0, 0)]
    assert misses(lines, warmup=8) == (1, [])


def test_other_lines_ignored():
    assert misses(["[d3d11] present", "ao: something else"]) == (0, [])


def test_frame_without_fit_is_not_a_miss():
    # ok=0: no usable depth fit, so the trigger never fires and never counts
    # the frame as missed; the checker must agree.
    lines = [line(i, 1, 4, 4, 4, 0) for i in range(1, 10)]
    lines += [line(10, 0, 4, 4, 0, 0, ok=0), line(11, 0, 4, 4, 0, 0)]
    steady, missed = misses(lines, warmup=8)
    assert steady == 3 and missed == [11]
