"""Compatibility wrapper for the T4 internal demo."""

from __future__ import annotations

import sys

from ssv_agent.demos.internal import main


if __name__ == "__main__":
    sys.exit(main())
