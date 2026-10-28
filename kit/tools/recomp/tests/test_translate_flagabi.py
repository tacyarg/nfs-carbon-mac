"""Flag liveness tables the lifter's dead-flag pass relies on."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import translate as T  # noqa: E402


def test_flag_readers_say_what_they_read():
    # LAHF copies SF ZF AF PF CF into AH; CMC flips CF.  An instruction missing
    # from the table counts as reading no flags, and the pass drops the writes.
    assert T.FLAG_EFFECT["LAHF"][1] == frozenset(("cf", "pf", "af", "zf", "sf"))
    assert T.FLAG_EFFECT["CMC"] == (frozenset(("cf",)), frozenset(("cf",)))


# ------------------------------------------------------------------ FLAGABI --
# Whole-program flag liveness (ported from nfsu2-web's tools/flagabi.py, MIT,
# Copyright (c) 2026 th0mk): MSVC code reads no flags across a call or a return,
# so they are dead there unless the table proves a caller reads them.

import pytest  # noqa: E402

from test_translate_insns import NoImage, Opts  # noqa: E402

A, B, C = 0x0D0F0000, 0x0D0F1000, 0x0D0F2000


def fn_of(addr, lines):
    text = "".join("%08x  %s\n" % (addr + off, t) for off, t in lines)
    fn = T.Function(addr, "FUN_%08x" % addr, lines[-1][0] + 1, T.parse_listing_text(text))
    fn.measure(NoImage())
    return fn


def translator(*fns):
    tr = T.Translator(NoImage(), {f.addr for f in fns}, Opts())
    for f in fns:
        tr.prepare(f)
    return tr


def caller_of_b():
    return fn_of(A, [(0, "CMP EAX,EBX"), (2, "JZ 0x%08x" % (A + 9)), (4, "CALL 0x%08x" % B),
                     (9, "RET")])


def plain_b():
    return fn_of(B, [(0, "XOR EAX,EAX"), (2, "RET")])


def test_flags_are_dead_at_calls_and_returns():
    a, b = caller_of_b(), plain_b()
    tr = translator(a, b)
    tr.flag_abi = ({A: T.NO_FLAGS, B: T.NO_FLAGS}, set())
    tr.flow_setup(a, [])
    live_out = tr.liveness_abi(a, [])[0]
    assert live_out[0] == frozenset(("zf",))        # the JZ reads ZF, nothing else survives
    assert live_out[1] == T.NO_FLAGS                 # B reads none at its entry
    text = "\n".join(tr.translate(a))
    assert "eflags_zf" in text and "eflags_cf" not in text


def test_a_callee_that_reads_flags_at_entry_keeps_them_live():
    a, b = caller_of_b(), plain_b()
    tr = translator(a, b)
    tr.flag_abi = ({A: T.NO_FLAGS, B: frozenset(("cf",))}, set())
    tr.flow_setup(a, [])
    assert tr.liveness_abi(a, [])[0][0] == frozenset(("zf", "cf"))


def test_a_flag_read_after_a_call_reaches_through_a_tail_jump():
    # A reads CF after calling B; B tail-jumps to C; C sets CF and returns to A.
    a = fn_of(A, [(0, "CALL 0x%08x" % B), (5, "JC 0x%08x" % (A + 9)), (7, "XOR EAX,EAX"),
                  (9, "RET")])
    b = fn_of(B, [(0, "JMP 0x%08x" % C)])
    c = fn_of(C, [(0, "CMP EAX,EBX"), (2, "RET")])
    tr = translator(a, b, c)
    T.flag_abi_table(tr, [a, b, c], {})
    assert tr.flag_abi is not None
    assert {B, C} <= tr.flag_abi[1]
    assert "eflags_cf" in "\n".join(tr.translate(c))


def test_an_alternate_entry_reads_what_its_own_instructions_read():
    e = A + 2
    a = fn_of(A, [(0, "CMP EAX,EBX"), (2, "JZ 0x%08x" % (A + 6)), (4, "XOR EAX,EAX"),
                  (6, "RET")])
    tr = translator(a)
    tr.func_addrs.add(e)
    T.flag_abi_table(tr, [a], {A: {e}})
    assert tr.flag_abi[0][e] == frozenset(("zf",))
    assert tr.flag_abi[0][A] == T.NO_FLAGS


def test_flags_read_after_an_indirect_call_turn_the_table_off():
    a = fn_of(A, [(0, "CALL EAX"), (2, "JZ 0x%08x" % (A + 6)), (4, "XOR EAX,EAX"), (6, "RET")])
    tr = translator(a)
    T.flag_abi_table(tr, [a], {})
    assert tr.flag_abi is None


def test_a_jump_the_interpreter_may_take_keeps_every_flag():
    # A jump to no known function goes through recomp_jump, and the
    # interpreter may run the target from the middle of its code.
    a = fn_of(A, [(0, "CMP EAX,EBX"), (2, "JMP 0x0d0f5000")])
    tr = translator(a)
    tr.flag_abi = ({A: T.NO_FLAGS}, set())
    tr.flow_setup(a, [])
    assert tr.liveness_abi(a, [])[0][0] == T.ALL_FLAGS


def test_a_body_that_now_reads_more_than_the_table_says_is_not_lifted():
    a = fn_of(A, [(0, "JZ 0x%08x" % (A + 4)), (2, "XOR EAX,EAX"), (4, "RET")])
    tr = translator(a)
    tr.flag_abi = ({A: T.NO_FLAGS}, set())          # built before A read ZF at entry
    with pytest.raises(T.TranslateError):
        tr.translate(a)


def test_a_virtual_call_written_as_a_tail_jump_leaves_no_flags_live():
    # MSVC ends many methods with JMP dword ptr [EAX + n]: a call to another
    # function's entry, which reads no flags.  Flags must not leak back to the
    # indirect call before it.
    a = fn_of(A, [(0, "CALL dword ptr [EDX + 0x30]"), (3, "MOV EAX,dword ptr [ECX]"),
                  (5, "JMP dword ptr [EAX + 0x38]")])
    tr = translator(a)
    T.flag_abi_table(tr, [a], {})
    assert tr.flag_abi is not None
    assert tr.liveness_abi(a, [])[0][0] == T.NO_FLAGS


def test_a_function_whose_flags_are_read_cannot_tail_jump_somewhere_unknown():
    a = fn_of(A, [(0, "CALL 0x%08x" % B), (5, "JC 0x%08x" % (A + 9)), (7, "XOR EAX,EAX"),
                  (9, "RET")])
    b = fn_of(B, [(0, "CMP EAX,EBX"), (2, "JMP dword ptr [EAX + 0x38]")])
    tr = translator(a, b)
    T.flag_abi_table(tr, [a, b], {})
    assert tr.flag_abi is None


def test_a_tail_jump_without_a_table_does_not_reach_every_flag_reader_in_the_body():
    # successors() lets a table-less indirect jump land on any instruction of
    # its body; for flags it is a call to another entry (nfsu2-web does the
    # same), or a JZ anywhere in the body keeps flags live at every such jump.
    a = fn_of(A, [(0, "TEST EAX,EAX"), (2, "JZ 0x%08x" % (A + 13)),
                  (4, "CALL dword ptr [EDX + 0x30]"), (7, "MOV EAX,dword ptr [ECX]"),
                  (9, "JMP dword ptr [EAX + 0x38]"), (13, "RET")])
    tr = translator(a)
    T.flag_abi_table(tr, [a], {})
    assert tr.flag_abi is not None
