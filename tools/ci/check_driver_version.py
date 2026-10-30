#!/usr/bin/env python3
"""Fail when driver-affecting files changed but the INF DriverVer did not rise.

Windows keys a published driver package on its INF. A new binary shipped with
an unchanged DriverVer is silently ignored: the old package is reused and the
upgrade then fails its payload check (seen on the test VM). This check runs in
CI so that cannot reach a release.

    check_driver_version.py [--base REV] [--head REV] [--repo DIR]

Without --base the base is the merge base with main (or HEAD~1 on main).
Exit status: 0 ok, 1 violation, 2 usage or git error.
"""
import argparse
import re
import subprocess
import sys

INF = "driver/chmlink.inf"

# Everything compiled into chmlink.sys, plus the INF itself. SDK-only files
# (include/chmlink/api.h, src/windows, tools, tests) do not change the driver.
DRIVER_FILES = (
    "driver/",
    "src/queue/",
    "src/protocol/",
    "src/packet/",
    "include/chmlink/config.h",
    "include/chmlink/device.h",
    "include/chmlink/packet.h",
    "include/chmlink/protocol.h",
    "include/chmlink/query.h",
    "include/chmlink/version.h",
)

DRIVER_VER = re.compile(r"^\s*DriverVer\s*=\s*[^,\s]+\s*,\s*(\d+)\.(\d+)\.(\d+)\.(\d+)\s*$", re.M)


class CheckError(Exception):
    pass


def git(repo, *args):
    result = subprocess.run(("git", "-C", repo) + args, capture_output=True, text=True)
    if result.returncode != 0:
        raise CheckError("git %s failed: %s" % (" ".join(args), result.stderr.strip()))
    return result.stdout


def rev_exists(repo, rev):
    return subprocess.run(("git", "-C", repo, "rev-parse", "--verify", "--quiet", rev + "^{commit}"),
                          capture_output=True).returncode == 0


def default_base(repo, head):
    for main in ("origin/main", "main"):
        if rev_exists(repo, main):
            base = git(repo, "merge-base", main, head).strip()
            if base != git(repo, "rev-parse", head).strip():
                return base
            break   # head is main itself: compare with its parent
    if rev_exists(repo, head + "~1"):
        return git(repo, "rev-parse", head + "~1").strip()
    raise CheckError("cannot determine a base revision; pass --base")


def driver_version(repo, rev):
    """DriverVer tuple at rev, or None when the INF does not exist there."""
    probe = subprocess.run(("git", "-C", repo, "cat-file", "-e", "%s:%s" % (rev, INF)), capture_output=True)
    if probe.returncode != 0:
        return None
    matches = DRIVER_VER.findall(git(repo, "show", "%s:%s" % (rev, INF)))
    if len(matches) != 1:
        raise CheckError("%s at %s must contain exactly one DriverVer, found %d" % (INF, rev, len(matches)))
    return tuple(int(part) for part in matches[0])


def affects_driver(path):
    return any(path == entry or (entry.endswith("/") and path.startswith(entry)) for entry in DRIVER_FILES)


C_SOURCES = (".c", ".h")


def strip_c_comments(text):
    """The text with C comments and insignificant whitespace removed.

    Comments do not reach the binary, so a comment-only edit must not demand a version bump. String
    and character literals are respected: "http://x" is code, and changing it changes the driver.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return " ".join("".join(out).split())


def code_changed(repo, base, head, path):
    """True when the file's compiled content differs. Anything but C sources is compared verbatim:
    Windows hashes the INF text, comments included."""
    def content(rev):
        probe = subprocess.run(("git", "-C", repo, "cat-file", "-e", "%s:%s" % (rev, path)), capture_output=True)
        return None if probe.returncode else git(repo, "show", "%s:%s" % (rev, path))
    before, after = content(base), content(head)
    if before is None or after is None or not path.endswith(C_SOURCES):
        return before != after
    return strip_c_comments(before) != strip_c_comments(after)


def check(repo, base, head):
    """Returns (ok, message)."""
    changed = [p for p in git(repo, "diff", "--name-only", base, head).splitlines() if p]
    touched = sorted(p for p in changed if affects_driver(p) and code_changed(repo, base, head, p))
    if not touched:
        return True, "No driver code changed (comment-only edits do not count); DriverVer not required."
    old, new = driver_version(repo, base), driver_version(repo, head)
    if new is None:
        return False, "%s is missing at %s." % (INF, head)
    shown = ", ".join(touched[:6]) + (" ..." if len(touched) > 6 else "")
    if old is None:
        return True, "%s is new (%s); nothing to compare." % (INF, ".".join(map(str, new)))
    fmt = lambda v: ".".join(map(str, v))
    if new == old:
        return False, ("Driver files changed (%s) but DriverVer is still %s. Windows reuses a published "
                       "package with an unchanged INF, so the new binary would not be installed. "
                       "Raise DriverVer in %s and CHMLINK_VERSION_* in include/chmlink/version.h."
                       % (shown, fmt(old), INF))
    if new < old:
        return False, "DriverVer went backwards: %s -> %s." % (fmt(old), fmt(new))
    return True, "DriverVer raised %s -> %s for: %s" % (fmt(old), fmt(new), shown)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", default=".")
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    args = parser.parse_args(argv)
    try:
        base = args.base
        if not base or set(base) == {"0"} or not rev_exists(args.repo, base):
            base = default_base(args.repo, args.head)
        # Compare with the point where head branched off, not the tip of the base branch, so
        # changes that landed on the base branch afterwards are not attributed to head.
        base = git(args.repo, "merge-base", base, args.head).strip() or base
        ok, message = check(args.repo, base, args.head)
    except CheckError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2
    print(("OK: " if ok else "FAIL: ") + message)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
