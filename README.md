# backtrace-track

## Requirements

- For backtrace resolution, `#include<execinfo.h>` (use `backtrace()` to get call stacks).
- For symbol resolution, must be compiled with `-rdynamic` flag (use `backtrace_symbols()` to resolve).
- [opt] For relative address resolution, must be compiled with `-ldl` flag (use `dl_addr()`).
- [opt] For source code resolution, must be compiled with `-g` flag and install `addr2line` from [GNU Binutils](https://www.gnu.org/software/binutils/).
- Recommeded to compile with `-O0` to disable function optimization.

Compile like:

```bash
# directly add to your code
g++ -o prog -O0 -g -rdynamic -ldl bttrack.cpp prog.cpp

# build as shared library
g++ -o libbttrack.so -O3 -g -ldl -shared -fPIC bttrack.cpp
g++ -o prog -O0 -g -rdynamic -L. -lbttrack prog.cpp
```

## Example

```bash
./runtest.sh test_001.cpp
```