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
  int64_t score;
};

// track all calls, we preserve 256 slots for different callers
void Record(uint8_t id, int64_t score = 1);

// dump all records
void Dump(uint8_t id, std::vector<StackFrames>& result);

// human readable string
std::string StackFramesToString(const std::vector<StackFrames>& records,
                                bool print_symbol = true);

// json string
std::string StackFramesToJson(const std::vector<StackFrames>& records,
                              int indent = 0);

}  // namespace bttrack
