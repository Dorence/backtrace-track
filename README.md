# backtrace-track

## Requirements

- For backtrace resolution, `#include<execinfo.h>` (use `backtrace()` to get call stacks).
- For symbol resolution, must be compiled with `-rdynamic` flag (use `backtrace_symbols()` to resolve).
- [opt] For relative address resolution, must be compiled with `-ldl` flag (use `dl_addr()`).
- [opt] For source code resolution, must be compiled with `-g` flag and install `addr2line` from [GNU Binutils](https://www.gnu.org/software/binutils/).
- Recommeded to compile with `-O0` to disable function optimization.

## Example

```bash
./runtest.sh test_001.cpp
```