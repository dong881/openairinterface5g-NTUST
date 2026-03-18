---
trigger: always_on
---

1. how to build vnf: cd ~/oai_mp_f_ming_develop_latest/openairinterface5g/cmake_targets/ran_build/build && sudo ninja nr-softmodem nr-uesoftmodem dfts ldpc params_libconfig rfsimulator
2. tab size: 2
A number of high-level comments:

- Indentation is two spaces, no tabs; try to limit the number of indentations.
- Line length is 132, not more than one statement per line; no whitespace at
  the end of lines
- The opening brace after a function is on a new line; after control flow
  statements (`if`, `while`, `switch`, ...), it is on the same line
- Pointer or reference operators (`*`, `&`) are right-aligned
- Do not commit code that is commented out
- Use strong typing (no `void *`, use complex data types such as `c16_t` over
  `uint32_t` in L1, ...)
- Do not use [magic numbers](https://en.wikipedia.org/wiki/Magic_number_(programming)#Unnamed_numerical_constants)
  for unnamed numerical constants and do not hardcode values
- Don't cast the result of `malloc()`: it is not needed, and can lead to bugs.
- Use `AssertFatal()` and `DevAssert()` to check for invariants, not for error
  handling: Assertions are for preventing bugs (e.g., unforeseen state),
  not to sanitize input.
- Use `const` on pointer function arguments that are input to that function;
  put output variables (via a pointer) last
- Do not do premature optimization; measure the code before writing SIMD
  instructions by hand, and measure again to show it is faster.
