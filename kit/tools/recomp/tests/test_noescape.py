"""Every runtime function the generated code passes the CPU to promises not to
keep it (RECOMP_NOESC).  One that does not makes the CPU escape, and clang then
writes registers back to the struct before every call in each function that
uses it: correct, but the speed of keeping them in locals is gone."""
import os
import re

RUNTIME = os.path.join(os.path.dirname(__file__), "..", "..", "..", "runtime")


def unmarked(path):
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return [m.group(0) for m in
            re.finditer(r"[(,]\s*(?:const\s+)?X86\s*\*\s*(?:restrict\s+)?(?!RECOMP_NOESC)\w+\s*[,)]", text)]


def test_every_cpu_parameter_the_generated_code_reaches_is_marked():
    for name in ("x86.h", "seh.h"):
        assert unmarked(os.path.join(RUNTIME, name)) == [], name


def test_the_runtime_builds_without_the_promise():
    text = open(os.path.join(RUNTIME, "x86.h")).read()
    assert "#ifndef RECOMP_NOESC\n#define RECOMP_NOESC\n#endif" in text
