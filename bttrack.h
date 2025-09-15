#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bttrack {

// function information at address
struct Frame {
  const void* addr;    // caller address
  const void* faddr;   // file base address
  std::string symbol;  // mangeled symbol name
  std::string func;    // function name
  std::string exec;    // executable name
  std::string file;    // source file name (?? if not available)
  int line;            // line of nearest symbol (-1 if not available)
};

// list of backtrace addresses
using FramePointers = std::vector<const void*>;

// stack frames and its count
struct StackFrames {
  std::vector<Frame*> frames;
  uint64_t count;
  int64_t score;
};

// track all calls, we preserve 256 slots for different callers
void Record(uint8_t id, int64_t score = 1);

// track provided backtrace, which can be obtained by GetBacktrace()
void Record(uint8_t id, const FramePointers& stack, int64_t score = 1);

// get current backtrace, return true if success
bool GetBacktrace(FramePointers& stack);

// dump all records
void Dump(uint8_t id, std::vector<StackFrames>& result);

// human readable string
std::string StackFramesToString(const std::vector<StackFrames>& records,
                                bool print_symbol = true);

// json string
std::string StackFramesToJson(const std::vector<StackFrames>& records,
                              int indent = 0);

}  // namespace bttrack
