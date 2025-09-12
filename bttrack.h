#pragma once

#include <string>
#include <vector>

namespace bttrack {

// function information at address
struct Frame {
  void* addr;
  void* faddr;         // file base address
  std::string symbol;  // mangeled symbol name
  std::string func;    // function name
  std::string exec;    // executable name
  std::string file;    // source file name
  int line;            // line of nearest symbol
};

// stack frames and its count
struct StackFrames {
  std::vector<Frame*> frames;
  uint64_t count;
};

// track all calls, we preserve 256 slots for different callers
void Record(uint8_t id);

// dump all records
void Dump(uint8_t id, std::vector<StackFrames>& result);

std::string PrintStackFrames(const std::vector<StackFrames>& records,
                             bool print_symbol = true);

}  // namespace bttrack
