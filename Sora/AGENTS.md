# Sora Source Navigation

This file complements the repository-level `../AGENTS.md` and applies to the Sora subtree.

`docs/code-index.json` is the machine-readable file-to-content map. `docs/code-index.md` is its compact human view.
Both files are generated from source metadata and must not be edited manually.

<!-- SORA_CPP_STYLE_START -->
## Sora C++ Style
This project opts into Sora C++ style. Load and apply `$sora-cpp-style` to all C++ implementation, refactoring,
review, testing, and documentation work until the user explicitly revokes this preference.
<!-- SORA_CPP_STYLE_END -->

Before introducing a facility, search the live index and the source tree:

```powershell
python tools/code_index.py query flags hash cpu
rg -n "candidate symbol" include src tests
```

After adding, removing, moving, or materially changing a source file, rebuild and verify the index:

```powershell
python tools/code_index.py build
python tools/code_index.py check
```

CMake exposes the non-default `Sora.CodeIndex.Check` target when a Python 3 interpreter is available. The generated
index is a navigation aid; source declarations, tests, and build metadata remain authoritative.
