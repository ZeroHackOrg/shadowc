"""shadowc - Shadow-Compile LLVM obfuscation tooling.

Layered packaging mirror of the repository's Split-Core architecture:

* ``shadowc.pipeline``   - clang -> opt -> llc orchestration with the pass plugin
* ``shadowc.cli``        - command line front-end
* ``shadowc.gatekeeper`` - Enterprise Vault licence gate (see src/gatekeeper.py)
"""

__version__ = "0.5.0"

__all__ = ["__version__"]