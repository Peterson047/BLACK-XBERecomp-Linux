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


def is_host(log):
    """The host says it started a server; the guest says it sent a join."""
    return "Server started on port" in log.path.read_text(errors="replace")


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

    # The death has to have crossed. When the test harness killed an enemy
    # on the host, the guest has to have applied the same damage to the same
    # actor: the same key, the same generation.
    kills = [fields for verb, fields, _raw in host.entries if verb == "test" and fields.get("what") == "kill"
             and fields.get("reason") == "applied"]
    if kills:
        kill = kills[-1]
        applied = [fields for verb, fields, _raw in guest.entries
                   if verb == "apply" and fields.get("kind") == "damage"]
        if not applied:
            problems.append("the host killed an actor and the guest applied no damage")
        else:
            same = [a for a in applied
                    if a.get("key_a") == kill.get("key_a") and a.get("key_b") == kill.get("key_b")
                    and a.get("generation") == kill.get("generation")]
            if not same:
                problems.append(
                    f"the guest did not apply the host's death: the host killed "
                    f"key={kill.get('key_a')}/{kill.get('key_b')} generation={kill.get('generation')}, "
                    f"the guest applied {[ (a.get('key_a'), a.get('key_b'), a.get('generation')) for a in applied ]}")
            else:
                for a in same:
                    if a.get("state") not in (None, "1", "2"):
                        problems.append(
                            f"the guest's actor is still alive after the host's death: state={a.get('state')}")

    # The body of the partner. When the guest made one, it has to be
    # animating, and when the partner fired, it has to have played the shot.
    body = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "created"]
    if body:
        clips = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "clip"]
        if not clips:
            problems.append("the guest made the body of the partner and it never picked a clip")
        else:
            names = {f.get("clip") for f in clips}
            if len(names) < 2:
                problems.append(f"the body of the partner never changed its clip: {sorted(names)}")
            # the shot: the host published one, so the body has to have played it
            shots = [f for v, f, _ in host.entries if v == "body" and f.get("what") == "publish"
                     and f.get("event") == "shot"]
            if shots:
                played = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "shot"]
                if not played:
                    problems.append("the host fired and the guest's body played no shot")
                states = {f.get("state") for f in clips}
                if states and "7" not in states:
                    problems.append(f"the body of the partner never held the state of the shot: {sorted(states)}")

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

    # An actor without a pair is a silent divergence.
    #
    # The count of the lines is not the gate: while the two copies of the
    # level load, one side has an actor the other does not have yet, and the
    # count goes up and down on its own. What matters is where it ended up:
    # the summary says how many are unpaired now, and the last pairing says
    # how the two sides line up at the end.
    missing = 0
    for name, log in (("host", host), ("guest", guest)):
        pairs = log.verbs("pair")
        if not pairs:
            continue
        last = pairs[-1]
        value = last.get("unpaired")
        if value is not None and value != "0":
            problems.append(f"the {name} ended with unpaired={value}")
        # A host actor the guest has not activated yet is not a pairing
        # failure: the two copies of the level bring their actors in at
        # their own pace. It is reported, and it is what the activation
        # replication is about.
        value = last.get("missing")
        if value is not None and value != "0":
            missing = max(missing, int(value))

    # The errors the co-op code reported itself.
    for name, log in (("host", host), ("guest", guest)):
        errors = log.verbs("error")
        if errors:
            problems.append(f"the {name} reported {len(errors)} co-op errors: {errors[0]}")

    # The guest is never a source: it must not publish events. The host
    # publishes, and its log says so.
    for name, log in (("host", host), ("guest", guest)):
        published = log.verbs("publish")
        if not published:
            continue
        if is_host(log):
            continue
        problems.append(f"the {name} published {len(published)} events; the guest is never a source")

    # The memory the guest took for the replayed events has to go back to the
    # guest's heap. A difference is a leak of the guest's address space.
    for name, log in (("host", host), ("guest", guest)):
        for fields in log.verbs("scratch"):
            allocated = fields.get("alloc")
            freed = fields.get("free")
            if allocated is None or freed is None:
                continue
            if allocated != freed:
                problems.append(
                    f"the {name} took {allocated} blocks of the guest's memory and gave "
                    f"{freed} back; the rest is a leak")

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

    # The events the two sides handled have to be the same number.
    #
    # The snapshots are not: the host starts publishing before the guest
    # arrives, so the host has sent more than the guest has received. The
    # guest must not have received more than the host sent.
    host_value = host_summary.get("events")
    guest_value = guest_summary.get("events")
    if host_value is not None and guest_value is not None and host_value != guest_value:
        problems.append(f"the two sides disagree about 'events': host={host_value} guest={guest_value}")

    host_snapshots = host_summary.get("snapshots")
    guest_snapshots = guest_summary.get("snapshots")
    if host_snapshots is not None and guest_snapshots is not None:
        if int(guest_snapshots) > int(host_snapshots):
            problems.append(
                f"the guest received more snapshots than the host sent: host={host_snapshots} guest={guest_snapshots}")

    # The death has to have crossed. When the test harness killed an enemy
    # on the host, the guest has to have applied the same damage to the same
    # actor: the same key, the same generation.
    kills = [fields for verb, fields, _raw in host.entries if verb == "test" and fields.get("what") == "kill"
             and fields.get("reason") == "applied"]
    if kills:
        kill = kills[-1]
        applied = [fields for verb, fields, _raw in guest.entries
                   if verb == "apply" and fields.get("kind") == "damage"]
        if not applied:
            problems.append("the host killed an actor and the guest applied no damage")
        else:
            same = [a for a in applied
                    if a.get("key_a") == kill.get("key_a") and a.get("key_b") == kill.get("key_b")
                    and a.get("generation") == kill.get("generation")]
            if not same:
                problems.append(
                    f"the guest did not apply the host's death: the host killed "
                    f"key={kill.get('key_a')}/{kill.get('key_b')} generation={kill.get('generation')}, "
                    f"the guest applied {[ (a.get('key_a'), a.get('key_b'), a.get('generation')) for a in applied ]}")
            else:
                for a in same:
                    if a.get("state") not in (None, "1", "2"):
                        problems.append(
                            f"the guest's actor is still alive after the host's death: state={a.get('state')}")

    # The body of the partner. When the guest made one, it has to be
    # animating, and when the partner fired, it has to have played the shot.
    body = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "created"]
    if body:
        clips = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "clip"]
        if not clips:
            problems.append("the guest made the body of the partner and it never picked a clip")
        else:
            names = {f.get("clip") for f in clips}
            if len(names) < 2:
                problems.append(f"the body of the partner never changed its clip: {sorted(names)}")
            # the shot: the host published one, so the body has to have played it
            shots = [f for v, f, _ in host.entries if v == "body" and f.get("what") == "publish"
                     and f.get("event") == "shot"]
            if shots:
                played = [f for v, f, _ in guest.entries if v == "body" and f.get("what") == "shot"]
                if not played:
                    problems.append("the host fired and the guest's body played no shot")
                states = {f.get("state") for f in clips}
                if states and "7" not in states:
                    problems.append(f"the body of the partner never held the state of the shot: {sorted(states)}")

    if problems:
        return fail("; ".join(problems))

    print(
        "coop log check ok: "
        f"epoch={host_summary.get('epoch')} "
        f"snapshots={host_summary.get('snapshots')} "
        f"events={host_summary.get('events')} "
        f"unpaired={host_summary.get('unpaired')} "
        f"divergent={host_summary.get('divergent')}"
        + (f" missing={missing}" if missing else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
