# lumyr-lang-compiler

A minimal compiled programming language implemented with Flex/Bison. Transpiles source to C and compiles to native binary. For compiler-principle learning.

## Features

- **Lexer & Parser**: Flex/Bison based, with full grammar support
- **AST & Semantic Analysis**: Type checking, symbol table, scope management
- **Intermediate Representation**: Bytecode IR with optimization passes
- **Dual Execution Modes**:
  - VM interpreter (fast startup)
  - C transpilation + native compilation (high performance)
- **Garbage Collector**: Mark-and-sweep GC with generational support
- **Module System**: `import` / `export` with dependency resolution
- **Standard Library**: String, Array, Map, JSON, HTTP, Crypto, Regex, Thread, Time
- **Cross-platform**: macOS, Windows (MinGW-w64), Linux

## Project Structure

```
lumyr-lang-compiler/
├── src/                    # Compiler source
│   ├── lex/                # Lexer (Flex)
│   ├── parse/              # Parser (Bison) + module import
│   ├── ast/                # AST nodes + semantic analysis
│   ├── ir/                 # Bytecode IR, optimizer, VM, C codegen
│   └── i18n/               # Internationalization (6 languages)
├── kit/                    # Lumyr Kit (sub-projects)
│   └── runtime/            # Runtime library (GC, value types, stdlib)
├── tests/                  # Test suite
├── third_party/            # Prebuilt dependencies (Windows)
├── Makefile                # Cross-platform build
├── build.sh / build.bat    # Build scripts (Unix / Windows)
├── regress_dual.sh / .bat / .py  # Dual-channel regression tests (Unix/Windows/Python)
```

## Building

### Prerequisites

- GCC / Clang
- GNU Make
- Flex 2.6+ and Bison 3.8+
- libcurl, libiconv, tre (for regex/HTTP/crypto)

### macOS / Linux

```bash
./build.sh build          # Build compiler
./build.sh check          # Check toolchain
./build.sh run            # Build and run REPL
```

### Windows

```cmd
build.bat build           # Build compiler
build.bat check           # Check toolchain
build.bat run             # Build and run REPL
```

## Usage

```bash
# Interpret (VM mode)
lumyr source.lm

# Compile to native binary
lumyr -c source.lm -o output

# Emit C source only
lumyr -S source.lm -o output.c
```

## Internationalization

The compiler supports 6 languages, auto-detected from system locale:

- Simplified Chinese (default)
- Traditional Chinese
- English
- Japanese
- Korean
- Hindi

Override with environment variable:

```bash
export LUMYR_LANG=en     # Force English
set LUMYR_LANG=ja        # Force Japanese (Windows)
```

## Testing

```bash
# Dual-channel regression (VM + compiled, compare stdout)
./regress_dual.sh        # Unix
python regress_dual.py   # Windows (recommended, reliable subprocess output capture)
regress_dual.bat         # Windows (legacy, may have output buffering issues)
```

## License

Licensed under the **Apache License, Version 2.0**.

```
Copyright 2026 杨川 Yang Chuan (yance-ai)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

See [LICENSE](LICENSE) for the full license text, and [NOTICE](NOTICE) for third-party component attribution.
