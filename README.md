# backtrace-track

## Requirements

- C++ 11 and above. Should work fine on most linux distributions.
- For backtrace resolution, `#include<execinfo.h>` (use `backtrace()` to get call stacks).
- For symbol resolution, must be compiled with `-rdynamic` flag (use `backtrace_symbols()` to resolve). If you have `addr2line` installed, this condition may be relaxed.
- [opt] For relative address resolution, must be compiled with `-ldl` flag (use `dl_addr()`).
- [opt] For source code resolution, must be compiled with `-g` flag and install `addr2line` from [GNU Binutils](https://www.gnu.org/software/binutils/).
- Recommeded to compile with `-O0` to disable function optimization.

## Build

Only need 1 header file `bttrack.h` and 1 source file `bttrack.cpp`. Compile like:

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

## Usage

- Basic usage (see `test_001.cpp`):
  - Record backtrace: `Record(id, score=1)`
  - Get recorded stack frames: `Dump(id, output)`
  - To human readable: `StackFramesToString(records, print_symbol=true)`
  - To JSON: `StackFramesToJson(records, indent=2)`
  - Example:

```c++
#include "bttrack.h"
#include <iostream>

int main() {
  // backtrace can be recorded in multiple channels
  const uint8_t id = 0;

  // record
  bttrack::Record(id);

  // dump result
  std::vector<bttrack::StackFrames> records;
  bttrack::Dump(id, records);

  // parse & print result
  std::string readable = bttrack::StackFramesToString(records);
  std::string json = bttrack::StackFramesToJson(records, 2);
  std::cout << readable << std::endl << json << std::endl;
  return 0;
}
```

- Advanced usage (see `test_002.cpp`):
  - Get backtrace: `GetBacktrace(stack)`
  - Manually record at anytime: `Record(id, stack, score=1)`

## LICENSE

MIT License. All rights reserved.
