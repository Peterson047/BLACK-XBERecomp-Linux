#!/usr/bin/env python3
"""The log checker has to be able to fail. These are the cases it must catch.

Run: python3 tests/coop/test_assert_coop_log.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECKER = HERE / "assert_coop_log.py"

GOOD_HOST = """\
coop: snapshot epoch=1 actors=9
coop: pair epoch=1 paired=9 unpaired=0 missing=0
coop: event epoch=1 seq=1 type=ActorDamaged
coop: summary epoch=1 snapshots=100 events=42 unpaired=0 divergent=0
"""

GOOD_GUEST = """\
coop: snapshot epoch=1 actors=9
coop: apply epoch=1 seq=1
coop: apply epoch=1 seq=2
coop: pair epoch=1 paired=9 unpaired=0 missing=0
coop: summary epoch=1 snapshots=100 events=42 unpaired=0 divergent=0
"""


def run(host_text, guest_text):
    with tempfile.TemporaryDirectory() as directory:
        host = Path(directory) / "host.log"
        guest = Path(directory) / "guest.log"
        host.write_text(host_text)
        guest.write_text(host_text if guest_text is None else guest_text)
        result = subprocess.run(
            [sys.executable, str(CHECKER), str(host), str(guest)],
            capture_output=True,
            text=True,
        )
        return result.returncode, result.stdout + result.stderr


CASES = [
    ("two matching sides pass", GOOD_HOST, GOOD_GUEST, 0),
    ("a missing summary fails", GOOD_HOST.replace(
        "coop: summary epoch=1 snapshots=100 events=42 unpaired=0 divergent=0\n", ""), GOOD_GUEST, 1),
    ("different rounds fail", GOOD_HOST,
     GOOD_GUEST.replace("epoch=1", "epoch=2"), 1),
    ("an unpaired actor fails", GOOD_HOST.replace("unpaired=0", "unpaired=3"), GOOD_GUEST, 1),
    ("a pairing that ends short fails", GOOD_HOST.replace("paired=9 unpaired=0 missing=0",
                                                          "paired=6 unpaired=3 missing=3"), GOOD_GUEST, 1),
    ("a pairing that ends missing fails", GOOD_HOST.replace("paired=9 unpaired=0 missing=0",
                                                             "paired=9 unpaired=0 missing=1"), GOOD_GUEST, 1),
    ("a divergence fails", GOOD_HOST.replace("divergent=0", "divergent=1"), GOOD_GUEST, 1),
    ("a co-op error fails", GOOD_HOST + "coop: error what=something\n", GOOD_GUEST, 1),
    ("out of order sequences fail", GOOD_GUEST,
     GOOD_GUEST.replace("coop: apply epoch=1 seq=2", "coop: apply epoch=1 seq=0"), 1),
    ("the guest publishing fails", GOOD_GUEST, GOOD_GUEST + "coop: publish epoch=1 seq=3\n", 1),
    ("disagreeing counters fail", GOOD_HOST,
     GOOD_GUEST.replace("events=42", "events=41"), 1),
    ("no coop lines at all fails", "nothing here\n", "nothing here\n", 1),
    # the death has to cross: the guest has to apply the host's damage to the
    # same actor
    ("a death that did not cross fails",
     GOOD_HOST + "coop: test what=kill reason=applied actor=9 key_a=11 key_b=22 generation=3\n",
     GOOD_GUEST, 1),
    ("a death that crossed passes",
     GOOD_HOST + "coop: test what=kill reason=applied actor=9 key_a=11 key_b=22 generation=3\n",
     GOOD_GUEST + "coop: apply kind=damage actor=9 key_a=11 key_b=22 generation=3 damage=1000 state=1\n", 0),
    ("a death that crossed to another actor fails",
     GOOD_HOST + "coop: test what=kill reason=applied actor=9 key_a=11 key_b=22 generation=3\n",
     GOOD_GUEST + "coop: apply kind=damage actor=9 key_a=11 key_b=99 generation=3 damage=1000 state=1\n", 1),
    ("an actor that is still alive after the death fails",
     GOOD_HOST + "coop: test what=kill reason=applied actor=9 key_a=11 key_b=22 generation=3\n",
     GOOD_GUEST + "coop: apply kind=damage actor=9 key_a=11 key_b=22 generation=3 damage=1000 state=0\n", 1),
]


def main():
    failures = 0
    for name, host_text, guest_text, expected in CASES:
        code, output = run(host_text, guest_text)
        if code != expected:
            failures += 1
            print(f"FAIL {name}: expected exit {expected}, got {code}")
            print("     " + output.strip().replace("\n", "\n     "))
        else:
            print(f"ok   {name}")
    if failures:
        print(f"\n{failures} case(s) failed")
        return 1
    print("\nall log checker cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
