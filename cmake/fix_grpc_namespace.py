#!/usr/bin/env python3
"""Normalize gRPC generated code to use the public ::grpc namespace."""

import pathlib
import sys

REPLACEMENTS = {
    "::grpc_impl::": "::grpc::",
    "::grpc::experimental::ClientBidiReactor": "::grpc::ClientBidiReactor",
    "::grpc::experimental::ServerBidiReactor": "::grpc::ServerBidiReactor",
    "::grpc::experimental::CallbackServerContext": "::grpc::CallbackServerContext",
}


def rewrite(path: pathlib.Path) -> None:
    text = path.read_text()
    updated = text
    for old, new in REPLACEMENTS.items():
        updated = updated.replace(old, new)
    if updated != text:
        path.write_text(updated)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("Usage: fix_grpc_namespace.py <file> [file ...]", file=sys.stderr)
        return 1
    for name in argv[1:]:
        rewrite(pathlib.Path(name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
