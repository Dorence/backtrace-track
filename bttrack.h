#pragma once

#include <string>
#include <vector>

namespace bttrack {

// function information at address
struct Frame {
  void* addr;
  std::string symbol;
  std::string func_name;
  std::string file_name;
  int line;
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
