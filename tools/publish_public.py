#!/usr/bin/env python3
"""Publish the current branch as a single source snapshot to the public mirror.

    python -I tools/publish_public.py [BRANCH]

The private repository keeps its full development history. The public mirror is
deliberately a one-commit snapshot of the current HEAD tree, so old commits
(such as earlier versions that contained game-specific artefacts) are not
published. This is a force-push to the public mirror only.

The private push is attempted first. If it fails, the public mirror is not
touched. If the public push fails, the private repository still has the commit.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git(*args: str, capture: bool = False) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        text=True,
        capture_output=capture,
        check=True,
    )
    return result.stdout.strip() if capture else ""


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    ap = argparse.ArgumentParser(description="Publish a single snapshot to the public mirror.")
    ap.add_argument("branch", nargs="?", help="branch to publish (default: current branch)")
    ap.add_argument("--private-remote", default="origin", help="private remote name")
    ap.add_argument("--public-remote", default="public", help="public remote name")
    ap.add_argument("--public-branch", default="main", help="branch on the public mirror")
    ap.add_argument("--message", help="snapshot commit message")
    ap.add_argument("--skip-private", action="store_true", help="do not push the private remote first")
    args = ap.parse_args()

    branch = args.branch or git("branch", "--show-current", capture=True)
    if not branch:
        sys.exit("[publish] error: detached HEAD; give a branch name")

    check = subprocess.run(
        [sys.executable, "-I", str(ROOT / "tools" / "check_repo.py")],
        cwd=ROOT,
    )
    if check.returncode:
        sys.exit("[publish] error: repository check failed; nothing was pushed")

    head = git("rev-parse", "HEAD", capture=True)
    tree = git("rev-parse", "HEAD^{tree}", capture=True)
    subject = git("log", "-1", "--format=%s", head, capture=True)
    message = args.message or f"Public snapshot of {branch} at {head[:12]}: {subject}"

    dirty = git("status", "--porcelain", "--untracked-files=no", capture=True)
    if dirty:
        print("[publish] note: uncommitted tracked changes are not in HEAD and will not be published")

    if not args.skip_private:
        print(f"[publish] pushing {branch} to {args.private_remote}")
        git("push", args.private_remote, f"{branch}:{branch}")

    snapshot = git("commit-tree", tree, "-m", message, capture=True)
    print(f"[publish] snapshot {snapshot[:12]} of {branch} ({head[:12]})")
    git(
        "push",
        "--force",
        args.public_remote,
        f"{snapshot}:refs/heads/{args.public_branch}",
    )
    print(
        f"[publish] public mirror {args.public_remote}/{args.public_branch} "
        f"is now the single snapshot {snapshot[:12]}"
    )
    print("[publish] private history is unchanged")


if __name__ == "__main__":
    main()
