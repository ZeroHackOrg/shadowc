"""Package view of the licence gatekeeper.

The canonical, dependency-free implementation lives at ``src/gatekeeper.py``
(the file referenced by the README). This module re-exports it so the package
namespace works in all import styles.
"""

# Ensure the repository's src/ directory is importable (works for source
# checkouts, editable installs and in-repo test runs).
import os as _os
import sys as _sys

_SRC = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
if _SRC not in _sys.path:
    _sys.path.insert(0, _SRC)

from gatekeeper import (  # noqa: E402,F401
    LicenseError,
    LicenseToken,
    ZeroHackLicenseGate,
    derive_token,
    new_salt,
)

__all__ = ["LicenseError", "LicenseToken", "ZeroHackLicenseGate", "derive_token", "new_salt"]