"""Native dependency preparation package.

The package exposes the policy, transaction and snapshot contracts used by the
CLI. Provider implementations remain internal to this package.
"""

from .contracts import SNAPSHOT_KEYS, DependencyResult, DependencySnapshot, write_snapshot
from .manager import DependencyManager, load_dependency_manager
from .policy import (
    DEFAULT_ONNXRUNTIME_VERSION,
    DEFAULT_OPENCV_VERSION,
    DependencyConfig,
    DependencyError,
    detect_gpu_vendors,
    expected_providers,
    resolve_profile,
    validate_profile,
    validate_provider_set,
)

__all__ = [
    "DEFAULT_ONNXRUNTIME_VERSION",
    "DEFAULT_OPENCV_VERSION",
    "SNAPSHOT_KEYS",
    "DependencyConfig",
    "DependencyError",
    "DependencyManager",
    "DependencyResult",
    "DependencySnapshot",
    "detect_gpu_vendors",
    "expected_providers",
    "load_dependency_manager",
    "resolve_profile",
    "validate_profile",
    "validate_provider_set",
    "write_snapshot",
]
