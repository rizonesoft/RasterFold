#!/usr/bin/env python3
"""The OpenCode campaign heartbeat's job store: CronCreate, CronList and CronDelete for a harness without them.

    python3 scripts/opencode-heartbeat.py create --session <id> --cron '3-59/5 * * * *' --prompt-file <path>
    python3 scripts/opencode-heartbeat.py list [--json]
    python3 scripts/opencode-heartbeat.py delete --job <id>
    python3 scripts/opencode-heartbeat.py due [--now <YYYY-MM-DDTHH:MM:SSZ>]

The store is build/opencode-heartbeat-jobs.json, changed only under the campaign guard's lock and replaced atomically.
`create` prints the new job's id. `list` prints every job with its id, session, cron, prompt, creation time and last
firing. `delete` exits 0 whether it removed the job or the job was already gone, and confirms a pending cancellation
(`campaign_guard.py pending-cancel`) that names it, because the store is the authority on which jobs exist. `due` is what the OpenCode plugin
polls: it prints a JSON list of the jobs whose cron matched a minute after their last firing (or their creation),
at most one firing per job per call however many minutes were missed, records those firings, and drops jobs older
than 7 days, as a recurring Claude Code job expires. A cron field is `*`, `N`, `A-B`, `A-B/S` or `*/S`; anything else
is refused at `create`. `due` fails safe: an unreadable store or lock prints `[]` (no prompt) and exits 0, while
`create`, `list` and `delete` exit 2 naming the reason.
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import secrets
import sys

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / "build" / "opencode-heartbeat-jobs.json"
EXPIRY = timedelta(days=7)
MAX_PROMPT_CHARS = 100_000
FIELDS = ((0, 59), (0, 23), (1, 31), (1, 12), (0, 6))
FIELD = re.compile(r"^(?:\*|(\d+)(?:-(\d+))?)(?:/(\d+))?$")
SESSION = re.compile(r"^[A-Za-z0-9_.:-]{1,200}$")
JOB_ID = re.compile(r"^ohb-[0-9a-f]{12}$")


class Refused(Exception):
    """The store or the arguments are wrong; nothing changed."""


def parse_cron(text):
    """The five fields as sets of allowed values, plus whether day-of-month and day-of-week are restricted."""
    if not isinstance(text, str):
        raise Refused("the cron expression must be text")
    parts = text.split()
    if len(parts) != 5:
        raise Refused(f"cron {text!r} needs exactly five fields")
    sets = []
    for part, (low, high) in zip(parts, FIELDS):
        match = FIELD.fullmatch(part)
        # `N/S` is refused: its meaning (N-max/S) differs between cron implementations.
        if not match or (match.group(3) and match.group(1) is not None and match.group(2) is None):
            raise Refused(f"cron field {part!r} is not one of *, N, A-B, A-B/S, */S")
        start, end, step = match.groups()
        step = int(step) if step else 1
        if part.startswith("*"):
            first, last = low, high
        else:
            first = int(start)
            last = int(end) if end is not None else first
        if not (low <= first <= last <= high) or step < 1:
            raise Refused(f"cron field {part!r} is outside {low}-{high}")
        sets.append(set(range(first, last + 1, step)))
    return sets, parts[2] != "*", parts[4] != "*"


def matches(cron, minute):
    (mins, hours, doms, months, dows), dom_set, dow_set = cron
    if minute.minute not in mins or minute.hour not in hours or minute.month not in months:
        return False
    dom_ok, dow_ok = minute.day in doms, (minute.weekday() + 1) % 7 in dows
    # Standard cron: when both day fields are restricted, either one matching is enough.
    return (dom_ok or dow_ok) if dom_set and dow_set else (dom_ok and dow_ok)


def stamp(moment):
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_time(text):
    try:
        return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except (TypeError, ValueError):
        raise Refused(f"{text!r} is not a UTC time like 2026-10-29T03:08:00Z") from None


def guard_lock():
    sys.path.insert(0, str(ROOT / "scripts"))
    import campaign_guard
    return campaign_guard._Lock(str(ROOT))


def load():
    try:
        with open(STORE, "rb") as handle:
            data = json.loads(handle.read().decode("utf-8"))
    except FileNotFoundError:
        return []
    except (OSError, ValueError, RecursionError) as exc:
        raise Refused(f"unreadable job store {STORE}: {exc}") from None
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("jobs"), list):
        raise Refused(f"malformed job store {STORE}: needs schema 1 and a jobs list")
    for job in data["jobs"]:
        if not (isinstance(job, dict) and JOB_ID.fullmatch(str(job.get("id", "")))
                and isinstance(job.get("session"), str) and SESSION.fullmatch(job["session"])
                and isinstance(job.get("prompt"), str) and job["prompt"]):
            raise Refused(f"malformed job in {STORE}: {str(job)[:80]}")
        parse_cron(job.get("cron"))
        parse_time(job.get("created_at"))
        if job.get("last_fired") is not None:
            parse_time(job["last_fired"])
    return data["jobs"]


def save(jobs):
    STORE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STORE.with_name(f"{STORE.name}.{os.getpid()}.tmp")
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump({"schema": 1, "jobs": jobs}, handle, indent=1)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, STORE)


def create(session, cron, prompt_file, now):
    if not SESSION.fullmatch(session or ""):
        raise Refused("the session id must be 1 to 200 letters, digits or _ . : -")
    parse_cron(cron)
    try:
        prompt = Path(prompt_file).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise Refused(f"unreadable prompt file {prompt_file}: {exc}") from None
    if not prompt.strip() or len(prompt) > MAX_PROMPT_CHARS:
        raise Refused(f"the prompt must be non-empty and at most {MAX_PROMPT_CHARS} characters")
    with guard_lock():
        jobs = load()
        job = {"id": "ohb-" + secrets.token_hex(6), "session": session, "cron": " ".join(cron.split()),
               "prompt": prompt, "created_at": stamp(now), "last_fired": None}
        jobs.append(job)
        save(jobs)
    return job["id"]


def delete(job_id):
    """Remove one job; True when this call removed it. The store is the authority on which jobs exist, so a job it
    no longer holds is gone: a pending cancellation naming it (`campaign_guard.py end`) is confirmed here."""
    with guard_lock():
        jobs = load()
        kept = [job for job in jobs if job["id"] != job_id]
        removed = len(kept) != len(jobs)
        if removed:
            save(kept)
    import campaign_guard
    try:
        if job_id in [str(job.get("cron_id")) for job in campaign_guard._pending_jobs(str(ROOT))]:
            print(campaign_guard.cancel_confirmed(str(ROOT), job_id))
    except campaign_guard.GuardError as exc:  # the job is gone either way; the record's own fault is reported
        print(f"opencode-heartbeat: delete: {exc}", file=sys.stderr)
    return removed


def due(now):
    """The jobs to fire now, each recorded as fired at its latest matching minute."""
    current = now.replace(second=0, microsecond=0)
    fired, kept, changed = [], [], False
    with guard_lock():
        for job in load():
            created = parse_time(job["created_at"])
            if now - created >= EXPIRY:
                changed = True
                continue
            after = max(created, parse_time(job["last_fired"])) if job.get("last_fired") else created
            cron = parse_cron(job["cron"])
            slot = current
            while slot > after and not matches(cron, slot):
                slot -= timedelta(minutes=1)
            if slot > after:
                job = dict(job, last_fired=stamp(slot))
                fired.append({"id": job["id"], "session": job["session"], "prompt": job["prompt"],
                              "slot": stamp(slot)})
                changed = True
            kept.append(job)
        if changed:
            save(kept)
    return fired


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("create")
    make.add_argument("--session", required=True)
    make.add_argument("--cron", required=True)
    make.add_argument("--prompt-file", required=True)
    make.add_argument("--now")
    show = commands.add_parser("list")
    show.add_argument("--json", action="store_true")
    drop = commands.add_parser("delete")
    drop.add_argument("--job", required=True)
    poll = commands.add_parser("due")
    poll.add_argument("--now")
    args = parser.parse_args(argv)
    if args.command == "due":
        try:
            now = parse_time(args.now) if args.now else datetime.now(timezone.utc)
            print(json.dumps(due(now)))
        except Exception as exc:  # fail safe: no job fires on a store or lock it cannot trust
            print(f"opencode-heartbeat: due: {exc}", file=sys.stderr)
            print("[]")
        return 0
    try:
        if args.command == "create":
            now = parse_time(args.now) if args.now else datetime.now(timezone.utc)
            print(create(args.session, args.cron, args.prompt_file, now))
        elif args.command == "list":
            jobs = load()
            if args.json:
                print(json.dumps(jobs, indent=1))
            else:
                for job in jobs:
                    print(f"{job['id']} session={job['session']} cron={job['cron']!r} created={job['created_at']} "
                          f"last_fired={job.get('last_fired') or '-'}")
                    print(f"  prompt: {json.dumps(job['prompt'])}")
                if not jobs:
                    print("no jobs")
        else:
            print(f"deleted {args.job}" if delete(args.job) else f"{args.job} already gone")
        return 0
    except Exception as exc:
        print(f"opencode-heartbeat: {args.command}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
