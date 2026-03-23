#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


DEPS: dict[str, str] = {
    "libuv": "https://github.com/libuv/libuv.git",
    "libcoro": "https://github.com/jbaldwin/libcoro.git",
}


def run(cmd: list[str], cwd: Path | None = None) -> None:
    pretty = " ".join(cmd)
    print(f"+ {pretty}")
    subprocess.run(cmd, cwd=cwd, check=True)


def sync_repo(name: str, url: str, dst_root: Path, update: bool) -> None:
    dst = dst_root / name
    git_dir = dst / ".git"

    if not dst.exists():
        run(["git", "clone", "--depth", "1", "--recurse-submodules", url, str(dst)])
        return

    if not git_dir.exists():
        raise RuntimeError(f"{dst} exists but is not a git repo, please clean it first.")

    if not update:
        print(f"= {name}: exists, skip (use --update to pull latest)")
        return

    run(["git", "-C", str(dst), "pull", "--ff-only"])
    run(["git", "-C", str(dst), "submodule", "update", "--init", "--recursive"])


def main() -> None:
    parser = argparse.ArgumentParser(description="Bootstrap source dependencies.")
    sub = parser.add_subparsers(dest="command", required=True)

    deps = sub.add_parser("deps", help="clone/pull libuv and libcoro sources")
    deps.add_argument("--update", action="store_true", help="pull latest for existing repos")

    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    third_party = project_root / "third_party"
    third_party.mkdir(parents=True, exist_ok=True)

    if args.command == "deps":
        for name, url in DEPS.items():
            sync_repo(name=name, url=url, dst_root=third_party, update=args.update)

        print("Dependencies are ready in third_party/.")


if __name__ == "__main__":
    main()
