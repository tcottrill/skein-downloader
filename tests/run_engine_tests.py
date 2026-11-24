"""Runs the engine test executable against the local test server.

Usage: run_engine_tests.py <path to engine_tests>
"""
import subprocess
import sys

import engine_server


def main():
    if len(sys.argv) != 2:
        print("usage: run_engine_tests.py <engine_tests path>", file=sys.stderr)
        return 2
    server, base = engine_server.start()
    try:
        return subprocess.run([sys.argv[1], base]).returncode
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
