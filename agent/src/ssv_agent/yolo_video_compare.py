"""Compatibility wrapper for the OpenCV YOLO comparison demo."""

from __future__ import annotations

import sys

from ssv_agent.demos.yolo_compare import main


if __name__ == "__main__":
    sys.exit(main())
