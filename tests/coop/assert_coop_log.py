#!/usr/bin/env python3
"""Check the two instances' co-op logs against each other.

The game logs a fixed vocabulary so this can be a gate and not a demo:

    coop: <verb> key=value key=value ...

The verbs the checker knows are `snapshot`, `event`, `apply`, `pair`,
`unpaired`, `divergence`, `objective`, `interaction`, `mission`, `error` and
`summary`. A session ends with one line:

    coop: summary epoch=... snapshots=... events=... unpaired=... divergent=...

Usage:
    tests/coop/assert_coop_log.py <host.log> <guest.log>

Exit code 0 when the two sides agree, 1 when they do not, 2 when the logs do
not carry what the checker needs. The reason is printed either way.
"""

import re
import sys
from pathlib import Path

# coop: verb key=value key=value
#
# The game writes a prefix in front of every line (the time, the level, the
# thread), so the vocabulary is looked for inside the line, not at its start.
LINE = re.compile(r"\bcoop:\s+(?P<verb>[a-z]+)\s*(?P<rest>.*)$")
PAIR = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S*)")

# The verbs that carry a round.
ROUND_VERBS = {"snapshot", "event", "apply", "pair", "unpaired", "divergence",
               "objective", "interaction", "mission"}


class Log:
    def __init__(self, path):
        self.path = Path(path)
        self.entries = []
        if not self.path.exists():
            raise FileNotFoundError(path)
        for raw in self.path.read_text(errors="replace").splitlines():
            match = LINE.search(raw.strip())
            if not match:
                continue
            verb = match.group("verb")
            fields = dict(PAIR.findall(match.group("rest")))
            self.entries.append((verb, fields, raw))

    def verbs(self, name):
        return [f for v, f, _ in self.entries if v == name]

    def summary(self):
        summaries = self.verbs("summary")
        return summaries[-1] if summaries else None

    def epochs(self):
        found = set()
        for verb, fields, _ in self.entries:
            if verb in ROUND_VERBS and "epoch" in fields:
                found.add(fields["epoch"])
        return found


def fail(message):
    print(f"COOP LOG CHECK FAILED: {message}")
    return 1


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        host = Log(argv[1])
        guest = Log(argv[2])
    except FileNotFoundError as error:
        print(f"COOP LOG CHECK: no such log: {error}")
        return 2

    problems = []

    for name, log in (("host", host), ("guest", guest)):
        if not log.entries:
            problems.append(f"the {name} log has no 'coop:' lines at all")
        if log.summary() is None:
            problems.append(f"the {name} log has no 'coop: summary' line")

    if problems:
        return fail("; ".join(problems))

    host_summary = host.summary()
    guest_summary = guest.summary()

    # Both sides have to be in the same round.
    if host_summary.get("epoch") != guest_summary.get("epoch"):
        problems.append(
            f"the two sides are in different rounds: host epoch={host_summary.get('epoch')} "
            f"guest epoch={guest_summary.get('epoch')}")

    # The counters that have to be zero.
    for name, summary in (("host", host_summary), ("guest", guest_summary)):
        for counter in ("unpaired", "divergent"):
            value = summary.get(counter)
            if value is None:
                problems.append(f"the {name} summary has no '{counter}' counter")
            elif value not in ("0",):
                problems.append(f"the {name} summary reports {counter}={value}")

    # An actor that never found its pair is a silent divergence.
    for name, log in (("host", host), ("guest", guest)):
        unpaired = log.verbs("unpaired")
        if unpaired:
            problems.append(f"the {name} reported {len(unpaired)} unpaired actors")

    # The errors the co-op code reported itself.
    for name, log in (("host", host), ("guest", guest)):
        errors = log.verbs("error")
        if errors:
            problems.append(f"the {name} reported {len(errors)} co-op errors: {errors[0]}")

    # The guest is never a source: it must not publish events.
    for name, log in (("host", host), ("guest", guest)):
        published = log.verbs("publish")
        if published:
            problems.append(f"the {name} published {len(published)} events; the guest is never a source")

    # The sequences the guest applied have to be strictly increasing.
    sequences = []
    for _verb, fields, _raw in guest.entries:
        if "seq" in fields:
            try:
                sequences.append(int(fields["seq"]))
            except ValueError:
                problems.append(f"the guest reported a sequence that is not a number: {fields['seq']}")
    for before, after in zip(sequences, sequences[1:]):
        if after <= before:
            problems.append(f"the guest applied the sequences out of order: {before} then {after}")
            break

    # The counters of the two sides have to agree.
    for counter in ("snapshots", "events"):
        host_value = host_summary.get(counter)
        guest_value = guest_summary.get(counter)
        if host_value is not None and guest_value is not None and host_value != guest_value:
            problems.append(
                f"the two sides disagree about '{counter}': host={host_value} guest={guest_value}")

    if problems:
        return fail("; ".join(problems))

    print(
        "coop log check ok: "
        f"epoch={host_summary.get('epoch')} "
        f"snapshots={host_summary.get('snapshots')} "
        f"events={host_summary.get('events')} "
        f"unpaired={host_summary.get('unpaired')} "
        f"divergent={host_summary.get('divergent')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
