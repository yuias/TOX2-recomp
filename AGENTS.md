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

## C/C++ formatting

- Format C/C++ under `runtime/` and `tools/` with `clang-format` and the repository's `.clang-format`
  (Google style with 4-space indentation, 120 columns). `generated/` and `externals/` are not formatted.
- The Google Style Guide below applies to naming only; layout follows `.clang-format`.

## C/C++ Naming conventions

Google C++ Style Guide (https://google.github.io/styleguide/cppguide.html) with the following modifications.

| Kind | Convention | Example |
| --- | --- | --- |
| Variables (incl. arguments) | `snake_case` | `file_path` |
| Local variables | `snake_case` preferred; `camelCase` acceptable | `file_path` |
| Functions | `camelCase` | `openFile()` |
| Types (class, struct, enum, enum class, typedef, using) | `PascalCase` | `FileReader` |
| Concepts | `PascalCase` | `Readable` |
| Class data members | `snake_case` + trailing underscore | `file_path_` |
| Struct data members | `snake_case` | `file_path` |
| Constants (`constexpr`, etc.) | `k` + `PascalCase` | `kFilePath` |
| Enumerators | `PascalCase` | `NotFound` |
| Macros (discouraged) | `ALL_CAPS_WITH_UNDERSCORES` | `LOG_ERROR` |

### Consistency

- Local variable style must be consistent within a function and, preferably, within a file. Match the surrounding code.

### Exceptions

- Deviate when aligning with STL or external library conventions,
  e.g. `begin()`, `end()`, `value_type`, `iterator_category`.
- Types intended to satisfy STL concepts follow STL naming.
