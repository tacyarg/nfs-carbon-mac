"""X87OPT: a straight run of x87 instructions keeps the stack in C locals and writes
fpu_top, the tags and the exact flags back once, where other code can see them.

After nfsu2-web tools/lift.py X87Region (MIT, Copyright (c) 2026 th0mk; portions
(c) 2026 Talha Muderrisoglu).  test_translate_insns.py runs every x87 case with it on."""

from test_translate_flagabi import T, fn_of, translator

A, B = 0x0D0F0000, 0x0D0F1000


def lifted(fn, *others):
    tr = translator(fn, *others)
    tr.x87opt = True
    return "\n".join(tr.translate(fn))


def test_an_x87_run_keeps_the_stack_in_locals():
    text = lifted(fn_of(A, [(0, "FLD double ptr [ESP + 0x4]"), (4, "FMUL double ptr [ESP + 0x4]"),
                            (8, "FSTP double ptr [ESP + 0xc]"), (12, "RET")]))
    assert "fpush(c" not in text and "fset(c" not in text and "ST(c" not in text
    assert text.count("c->fpu_top") == 1          # read once; FLD then FSTP leave the depth as it was


def test_a_call_sees_the_stack_written_back():
    text = lifted(fn_of(A, [(0, "FLD double ptr [ESP + 0x4]"), (4, "CALL 0x%08x" % B),
                            (9, "FSTP double ptr [ESP + 0xc]"), (13, "RET")]), fn_of(B, [(0, "RET")]))
    assert text.index("c->fpu_top = ") < text.index("CALL_FN(")


def test_a_run_never_gives_two_slots_one_register():
    xr = T.X87Region()
    pushes = [xr.translate(["fpush(c, 1.0);"]) for _ in range(9)]
    assert all(p is not None for p in pushes[:8]) and pushes[8] is None


def test_a_scaled_index_in_the_listing_comment_still_rewrites():
    text = lifted(fn_of(A, [(0, "FLD float ptr [EAX*0x4 + 0x9b0000]"), (7, "FSTP float ptr [ESP + 0x4]"),
                            (11, "RET")]))
    assert "fpush(c" not in text and "fdrop(c" not in text


def test_a_shape_it_cannot_rewrite_ends_the_run_instead():
    assert T.X87Region().translate(["{ fset(c, 0, 1.0); fdrop(c); }"]) is None
