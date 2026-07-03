#!/usr/bin/env python3
"""A tiny hello-world in Python."""

import sys

def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "world"
    print(f"hello, {name}!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
