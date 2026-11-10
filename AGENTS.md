# Development rules

## Comment style

- Write code comments in English. Explain why, not what; skip comments on
  obvious code; update comments when you change the code they describe; do not
  add comments that only mark generated or modified sections. A language's own
  conventions take precedence.
- State things literally. Do not replace a plain phrase with a metaphor or
  flourish. Figurative wording adds connotations you did not choose and makes
  the reader decode it.
- Prefer `//` over `/* */`. Both are acceptable, but be consistent within a file.

## C/C++ style

Follow the Google C++ Style Guide, with these naming deviations:

- Functions and methods: `camelCase` (e.g. `openFile()`), not PascalCase.
- Enumerators: `PascalCase` (e.g. `NotFound`), no `k` prefix.
- Local variables: `snake_case` preferred; `camelCase` acceptable.
  Keep one style within a function, preferably within a file. Match the surrounding code.
- Constants whose value is obtained at run time (`static const` counter frequency, page size,
  a lazily created handle): variable naming (`cycles_per_ms`, `g_start`). `k` marks values fixed in source.
- Names emitted by the recompiler (`fn_%08x`, `imp_%08x`, `kTox2*` in the shared C headers)
  keep the recompiler's scheme.

Formatting is handled by clang-format. Naming is checked by clang-tidy (`.clang-tidy`);
fix reported violations instead of suppressing them.

Exception: types meant to satisfy STL concepts, members mirroring STL interfaces
(`push_back`, `value_type`, ...), and names mirroring an external API (FFmpeg functions,
`CELL_GCM_*` formats) follow that naming. Wrap such classes in
`// NOLINTBEGIN(readability-identifier-naming)` / `// NOLINTEND(readability-identifier-naming)`,
or mark a single line with `// NOLINT(readability-identifier-naming)` (`NOLINTNEXTLINE` when it
does not fit). C has no `constexpr`, so an anonymous `enum { kName = ... }` used as a constant in
C code is marked the same way.

## C/C++ formatting

- Format C/C++ under `runtime/` and `tools/` with `clang-format` and the repository's `.clang-format`
  (Google style with 4-space indentation, 120 columns). `generated/` and `externals/` are not formatted.

## C/C++ naming checks

- Check C/C++ under `runtime/` and `tools/` with `clang-tidy` and the repository's `.clang-tidy`.
  `generated/`, `externals/` and files generated into the build directory are not checked;
  the header filters in `.clang-tidy` drop their headers.
