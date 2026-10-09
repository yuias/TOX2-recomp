# Development rules

## Comment style

- Write concise code comments
  - Focus on the "why" rather than the "what".
  - Avoid stating the obvious; only explain non-trivial logic or edge cases.
- Keep comments up to date when changing the code they describe.
- Do not add comments that merely mark generated or modified sections.
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
