"""Compatibility wrapper for the T4 video demo adapter."""

from __future__ import annotations

import sys

from ssv_agent.demos.video import main


if __name__ == "__main__":
    sys.exit(main())
