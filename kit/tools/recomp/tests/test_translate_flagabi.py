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
