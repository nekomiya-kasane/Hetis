# Task Plan: x64-asan failing test investigation

## Goal
Diagnose and fix each failing x64-asan test, distinguishing implementation bugs from test bugs (most likely impl bugs per user's note).

## Failing tests
1. 122 - Abs works for float, double, int — constexpr
2. 126 - Lerp/Sign/Radians/Degrees — constexpr
3. 133 - Min/Max variadic fold — constexpr
4. 140 - Clamp/Saturate — constexpr
5. 141 - CopySign transfers sign — constexpr
6. 535 - Logger: multi-thread concurrent logging
7. 656 - dump → parse round-trip preserves content
8. 691 - PolymorphismTest_NOT_BUILT — not built (rebuild)
9. 712 - ABITest_NOT_BUILT — not built (rebuild)
10. 871 - Generator exception propagation
11. 881 - PackedColorA preserves alpha and clamps it into range
12. 887 - XYZ helpers preserve CIE XYZ and luminance semantics

## Phases
- [x] Phase 1: Set up plan file
- [x] Phase 2: Triage — find owning test files for each failure
- [x] Phase 3: Reproduce each failure individually under ASan
- [x] Phase 4: Diagnose root cause per failure
- [ ] Phase 5: Fix in priority order
- [ ] Phase 6: Re-run full suite verifying no regressions

## Root cause — confirmed

**This is NOT an implementation bug.** All 12 listed failures fall into two buckets:

1. **Test name encoding mismatch in ctest discovery on Windows.** Test cases whose names contain non-ASCII characters (em-dash `—` U+2014, arrow `→` U+2192) are registered into ctest with their UTF-8 byte sequence. When ctest then re-invokes Catch2 with `-R` filter, the bytes go through the Windows console code page (cp936/GB18030 in this user's locale), garbling the filter, and Catch2 reports "no match". Verified evidence:
   - ScalarMathTest binary lists all five suspect tests by name (em-dash visible).
   - Direct invocation `Test.Core.ScalarMathTest.exe "Abs works for*"` -> 6 assertions pass; `"CopySign*"` -> 3 assertions pass.
   - ctest output: `No test cases matched '"Abs works for float\, double\, int � constexpr"'`.
   - Tests 122/126/133/140/141 (em-dash) and 656 (`→`) are all in this bucket.
2. **NOT_BUILT artefacts.** 691 `Test.Core.PolymorphismTest_NOT_BUILT-b12d07c` and 712 `Test.Core.ABITest_NOT_BUILT-b12d07c` are stale ctest entries from an earlier configure when those binaries didn't exist. Rebuilt under x64-asan; both binaries now present.

Still TBD whether 535 / 871 / 881 / 887 are real. Invoking each directly to verify.

## Status
**Currently Phase 5** — verifying remaining four directly, then fixing the root cause via CMake encoding.

