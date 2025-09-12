#include "bttrack.h"

#include <cxxabi.h>
#include <execinfo.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace bttrack {

// array of stack pointers
struct Stack {
  const std::vector<void*> addrs;
  Stack(void** addrs_, int num) : addrs(addrs_, addrs_ + num) {}
  bool operator==(const Stack& other) const { return addrs == other.addrs; }
  bool operator<(const Stack& other) const {
    return addrs < other.addrs;  // lexicographical order
  }
};

// track all calls
class Tracker {
 public:
  // max stack frames to record
  static const int kMaxStackFrames = 256;
  // skip Tracker::Record() and Record()
  static const int kSkipFrames = 2;

  Tracker() = default;
  ~Tracker() = default;

  void Record();
  void Dump(std::vector<StackFrames>&);

 private:
  mutable std::mutex mutex_;
  // find frame according to addr
  std::unordered_map<void*, Frame> all_frames_;
  // stack frames and its count
  std::map<Stack, uint64_t> all_records_;

  // batch resolve addr to frame, should hold lock
  void Resolve(const std::vector<void*>&, std::vector<Frame*>&);
};

static Tracker& GetInstance(uint8_t id) {
  static Tracker instance[256];
  return instance[id];
}

void Dump(uint8_t id, std::vector<StackFrames>& records) {
  GetInstance(id).Dump(records);
}

void Record(uint8_t id) { GetInstance(id).Record(); }

/**
 * find "__libc_start_main" in
 * "/lib/x86_64-linux-gnu/libc.so.6(__libc_start_main+0xeb) [0x7f059d5a809b]""
 */
bool find_func_in_symbol(const char* symbol, size_t& start, size_t& end) {
  if (!symbol) {
    return false;
  }
  start = 0;
  while (symbol[start] && symbol[start] != '(') {
    start++;
  }
  if (!symbol[start]) {
    return false;
  }
  end = ++start;  // '(' found, move to next char
  while (symbol[end] && symbol[end] != '+') {
    end++;
  }
  if (!symbol[end]) {
    return false;
  }
  return end > start;  // substr not empty
}

// resolve address to frame information
Frame resolve_symbol(void* address, char* symbol) {
  Frame frame;
  frame.addr = address;

  size_t start = std::string::npos;
  size_t end = std::string::npos;
  bool found = find_func_in_symbol(symbol, start, end);
  if (found) {
    frame.symbol = symbol;
    symbol[end] = '\0';  // null-terminate the substr
    int status;
    char* demangled =
        abi::__cxa_demangle(symbol + start, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      frame.func_name = demangled;
      free(demangled);
    } else {
      frame.func_name.assign(symbol + start);
    }
  } else if (symbol) {
    // function name symbol not found
    frame.symbol = symbol;
    frame.func_name = symbol;
  } else {
    frame.symbol = "(no symbol)";
    frame.func_name = "__unknown__";
  }

  // @todo: addr2line
  if (start > 1) {
    frame.file_name.assign(symbol, start - 1);
  } else {
    frame.file_name = "__unknown__";
  }
  frame.line = -1;

  // printf("[resolve] \"%s\" -> %s at %s:%d @ %p \n", frame.symbol.c_str(),
  //        frame.func_name.c_str(), frame.file_name.c_str(), frame.line,
  //        frame.addr);
  return frame;
}

void Tracker::Record() {
  void* addrs[kMaxStackFrames];
  // @ref https://linux.die.net/man/3/backtrace
  int num_frames = backtrace(addrs, kMaxStackFrames);
  assert(num_frames > kSkipFrames);
  Stack stack_frames(addrs + kSkipFrames, num_frames - kSkipFrames);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    all_records_[stack_frames]++;  // find or create
  }
}

void Tracker::Dump(std::vector<StackFrames>& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  result.clear();

  // sort Stack* by count
  std::vector<std::pair<uint64_t, const Stack*>> sort_idx;
  sort_idx.reserve(all_records_.size());
  for (const auto& it : all_records_) {
    sort_idx.emplace_back(it.second, &it.first);
  }
  std::sort(sort_idx.begin(), sort_idx.end(), [](const auto& a, const auto& b) {
    return (a.first == b.first) ? (a.second < b.second) : (a.first > b.first);
  });

  // convert Stack* to StackFrames
  result.resize(sort_idx.size());
  for (size_t i = 0; i < sort_idx.size(); i++) {
    result[i].count = sort_idx[i].first;
    Resolve(sort_idx[i].second->addrs, result[i].frames);
  }
}

void Tracker::Resolve(const std::vector<void*>& addr,
                      std::vector<Frame*>& frames) {
  std::vector<size_t> not_found;
  not_found.reserve(addr.size());

  frames.clear();
  frames.resize(addr.size());

  // find if in all_frames_
  for (size_t i = 0; i < addr.size(); i++) {
    auto it = all_frames_.find(addr[i]);
    if (it == all_frames_.end()) {
      not_found.emplace_back(i);
    } else {
      frames[i] = &it->second;
    }
  }

  // batch lookup for not found
  if (!not_found.empty()) {
    const size_t num_lookup = not_found.size();
    void* addr_lookup[num_lookup];
    for (size_t i = 0; i < num_lookup; i++) {
      addr_lookup[i] = addr[not_found[i]];
    }
    // @ref https://linux.die.net/man/3/backtrace_symbols
    // current address to symbol, internal malloc-ed
    char** symbols = backtrace_symbols(addr_lookup, num_lookup);
    for (size_t i = 0; i < num_lookup; i++) {
      void* a = addr_lookup[i];
      auto r = all_frames_.emplace(a, resolve_symbol(a, symbols[i]));
      frames[not_found[i]] = &(r.first->second);
    }
  }

  // check all set
  for (auto* frame : frames) {
    assert(frame != nullptr);
  }
}

void PrintStack(std::ostringstream& oss, const StackFrames& stack, double sum,
                bool print_symbol) {
  oss << "recorded " << stack.count << " times (" << (stack.count / sum * 100.0)
      << " %), stack:" << std::endl;
  for (size_t f = 0; f < stack.frames.size(); f++) {
    auto* frame = stack.frames[f];
    oss << "#" << f << (f < 10 ? "  " : " ") << frame->func_name << " at "
        << frame->file_name << ":" << frame->line;
    if (print_symbol) {
      oss << ", " << frame->symbol;
    }
    oss << std::endl;
  }
}

std::string PrintStackFrames(const std::vector<StackFrames>& records,
                             bool print_symbol) {
  if (records.empty()) {
    return "Report: no records.";
  }
  uint64_t sum = 0;
  for (const auto& it : records) {
    sum += it.count;
  }
  std::ostringstream oss;
  oss << "Report: total " << sum << " records, in " << records.size()
      << " different stack frames:" << std::endl;
  for (size_t i = 0; i < records.size(); i++) {
    const auto& it = records[i];
    oss << "[" << i << "] ";
    PrintStack(oss, it, sum, print_symbol);
    oss << std::endl;
  }
  return oss.str();
}

}  // namespace bttrack