#include "bttrack.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace bttrack {

static const char* kFuncUnknown = "<unknown>";

// array of stack pointers
struct Stack {
  const FramePointers addrs;
  Stack(void** addrs_, int num) : addrs(addrs_, addrs_ + num) {}
  Stack(const FramePointers& addrs) : addrs(addrs) {}
  bool operator==(const Stack& other) const { return addrs == other.addrs; }
  bool operator<(const Stack& other) const {
    return addrs < other.addrs;  // lexicographical order
  }
};

struct StackStat {
  uint64_t count;
  int64_t score;
};

class Tracker {
 public:
  // max stack frames to record
  static const int kMaxStackFrames = 256;
  // skip Tracker::Record() and Record()
  static const int kSkipFrames = 2;

  Tracker() = default;
  ~Tracker() = default;

  void Record(int64_t score);
  void RecordStack(const FramePointers& stack, int64_t score);
  void Dump(std::vector<StackFrames>&);

  static bool GetBacktrace(FramePointers& stack);

 private:
  mutable std::mutex mutex_;
  // find frame according to addr
  std::unordered_map<const void*, Frame> all_frames_;
  // stack frames and its statistics
  std::map<Stack, StackStat> all_records_;

  // batch resolve addr to frame, should hold lock
  void Resolve(const FramePointers&, std::vector<Frame*>&);
};

static Tracker& GetInstance(uint8_t id) {
  static Tracker instance[256];
  return instance[id];
}

void Dump(uint8_t id, std::vector<StackFrames>& records) {
  GetInstance(id).Dump(records);
}

// use O2/O3 will break Tracker::kSkipFrames
#define OPTIMIZE_O1 __attribute__((optimize("O1")))

void OPTIMIZE_O1 Record(uint8_t id, int64_t score) {
  GetInstance(id).Record(score);
}

void OPTIMIZE_O1 Record(uint8_t id, const FramePointers& stack, int64_t score) {
  GetInstance(id).RecordStack(stack, score);
}

bool OPTIMIZE_O1 GetBacktrace(FramePointers& stack) {
  constexpr uint8_t id = std::numeric_limits<uint8_t>::max();
  return GetInstance(id).GetBacktrace(stack);
}

#undef OPTIMIZE_O1

/**
 * empty symbol: func = kFuncUnknown, return false
 * demangle success: func = demangled, return true
 * demangle failed: func = symbol, return false
 */
bool demangle_symbol(std::string& func, const char* symbol) {
  if (!symbol) {
    func = kFuncUnknown;
    return false;
  }
  int status;
  char* demangled = abi::__cxa_demangle(symbol, nullptr, nullptr, &status);
  if (status != 0 || demangled == nullptr) {
    func.assign(symbol);
    return false;
  }
  func = demangled;
  free(demangled);
  return true;
}

// find addr2line using which or whereis
bool is_addr2line_available() {
  static bool avail = [] {
    FILE* pipe = popen("which addr2line", "r");
    if (!pipe) {
      pipe = popen("whereis -b addr2line | grep -q addr2line", "r");
      if (!pipe) {
        return false;
      }
    }
    char buffer[256];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      result += buffer;
    }
    int status = pclose(pipe);
    int code = WEXITSTATUS(status);
    bool ok = code == 0 && !result.empty();
    if (result[result.size() - 1] != '\n') {
      result += '\n';
    }
    if (!ok) {
      fprintf(stderr, "addr2line not found: returns %d output %s", code,
              result.c_str());
    } else {
      fprintf(stderr, "addr2line found: %s", result.c_str());
    }
    return ok;
  }();
  return avail;
}

void lookup_addr2line(Frame& frame) noexcept {
  static const bool unwind_inline = true;

  if (!is_addr2line_available() || frame.faddr == nullptr) {
    return;
  }
  void* offset = (void*)((char*)frame.addr - (char*)frame.faddr);
  bool get_func = frame.func == kFuncUnknown;
  // addr2line [-f] [-i] -e <exec> <offset>
  const size_t cmd_len = frame.exec.size() + 128;
  char cmd[cmd_len];
  snprintf(cmd, cmd_len - 1, "addr2line %s%s-e %s %p", (get_func ? "-f " : ""),
           (unwind_inline ? "-i " : ""), frame.exec.c_str(), offset);
  FILE* pipe = popen(cmd, "r");
  if (!pipe) {
    return;
  }
  char buffer[256];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  // printf("popen(\"%s\") -> \"%s\"\n", cmd, result.c_str());
  int status = pclose(pipe);
  int code = WEXITSTATUS(status);
  if (code != 0 || result.empty()) {
    fprintf(stderr, "addr2line failed: returns %d output %s", code,
            result.c_str());
    return;
  }

  // result: "file:line (discriminator 2)\n",
  bool parse_ok = false;
  size_t start = 0;
  if (get_func) {
    // first line is function name
    size_t fend = result.find('\n', start);
    if (fend != std::string::npos) {
      result[fend] = '\0';  // null-terminate the substr
      const char* func_str = result.c_str() + start;
      demangle_symbol(frame.func, func_str);
      start = fend + 1;  // move to next line
    }
  }
  // maybe multiple lines for optimized code, only use the first line
  size_t eol = result.find('\n', start);
  if (eol == std::string::npos) {
    eol = result.size();  // end of string
  }
  size_t colon = result.rfind(':', eol - 1);  // last ':'
  assert(eol > start);
  if (colon >= start && colon < eol) {
    // try parse line number
    size_t lstart = colon + 1;
    if (result[lstart] == '?') {
      frame.line = -1;
      parse_ok = true;
    } else {
      try {
        size_t lend = result.find_first_not_of("0123456789", lstart);
        if (lend == std::string::npos) {
          lend = eol;
        }
        frame.line = std::stoi(result.substr(colon + 1, lend - lstart));
        parse_ok = frame.line >= 0;
      } catch (...) {
        // ignore parse error
      }
    }
  }
  if (parse_ok) {
    frame.file = result.substr(start, colon - start);
  }
  // printf("lookup_addr2line: %s:%d\n", frame.file.c_str(), frame.line);
  // @todo add "-f" to retrieve function name if frame.func is kFuncUnknown
}

/**
 * find "__libc_start_main" in
 * "/lib/x86_64-linux-gnu/libc.so.6(__libc_start_main+0xeb) [0x7f059d5a809b]"
 * `start` points to next char of '(', `end` points to '+'
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
  frame.faddr = nullptr;

  size_t start = std::string::npos;
  size_t end = std::string::npos;
  bool found = find_func_in_symbol(symbol, start, end);
  if (found) {
    // found function name in symbols
    frame.exec.assign(symbol, start - 1);
    frame.symbol = symbol;
    symbol[end] = '\0';  // null-terminate the substr
    demangle_symbol(frame.func, symbol + start);
  } else if (symbol) {
    // no function name in symbol, however, it could be "prog(+0xabcd)"
    if (start > 1) {
      frame.exec.assign(symbol, start - 1);
    } else {
      frame.exec = "??";
    }
    frame.symbol = symbol;
    frame.func = kFuncUnknown;
  } else {
    frame.exec = "??";
    frame.symbol = "(nil)";
    frame.func = kFuncUnknown;
  }

  // remove ending " [0x7f64639e009b]"
  size_t addr_start = frame.symbol.find(") [0x");
  if (addr_start != std::string::npos) {
    frame.symbol.erase(addr_start + 1);
  }

  // get binary and its base address using dladdr()
  Dl_info dl_info;
  auto ret = dladdr(address, &dl_info);
  if (ret) {
    if (start > 1) {
      // check exec name
      assert(strncmp(dl_info.dli_fname, symbol, start - 1) == 0);
    }
    // printf("dladdr: %s@%p, %s@%p(+0x%lx)\n", dl_info.dli_fname,
    //        dl_info.dli_fbase, dl_info.dli_sname, dl_info.dli_saddr,
    //        (char*)dl_info.dli_saddr - (char*)dl_info.dli_fbase);
    frame.faddr = dl_info.dli_fbase;
  }

  // get source code using addr2line
  frame.file = "??";
  frame.line = -1;
  lookup_addr2line(frame);

  // printf("[resolve] \"%s\" -> %s at %s:%d @ %p \n", frame.symbol.c_str(),
  //        frame.func_name.c_str(), frame.file_name.c_str(), frame.line,
  //        frame.addr);
  return frame;
}

void Tracker::Record(int64_t score) {
  void* addrs[kMaxStackFrames];
  // @ref https://linux.die.net/man/3/backtrace
  int num_frames = backtrace(addrs, kMaxStackFrames);
  if (num_frames <= kSkipFrames) {
    assert(false);
    return;
  }
  Stack stack_frames(addrs + kSkipFrames, num_frames - kSkipFrames);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // find or create
    auto it = all_records_.find(stack_frames);
    if (it == all_records_.end()) {
      all_records_.emplace(stack_frames, StackStat{1, score});
    } else {
      it->second.count++;
      it->second.score += score;
    }
  }
}

void Tracker::RecordStack(const FramePointers& stack, int64_t score) {
  std::lock_guard<std::mutex> lock(mutex_);
  // find or create
  Stack stack_frames(stack);
  auto it = all_records_.find(stack_frames);
  if (it == all_records_.end()) {
    all_records_.emplace(stack_frames, StackStat{1, score});
  } else {
    it->second.count++;
    it->second.score += score;
  }
}

bool Tracker::GetBacktrace(FramePointers& stack) {
  void* addrs[kMaxStackFrames];
  stack.clear();
  // @ref https://linux.die.net/man/3/backtrace
  int num_frames = backtrace(addrs, kMaxStackFrames);
  if (num_frames <= kSkipFrames) {
    return false;
  }
  stack.assign(addrs + kSkipFrames, addrs + num_frames);
  return true;
}

void Tracker::Dump(std::vector<StackFrames>& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  result.clear();

  // sort Stack* by count, tuple[count, Stack*, score]
  using Tuple = std::tuple<uint64_t, const Stack*, int64_t>;
  std::vector<Tuple> sort_idx;
  sort_idx.reserve(all_records_.size());
  for (const auto& it : all_records_) {
    sort_idx.emplace_back(it.second.count, &it.first, it.second.score);
  }
  std::sort(sort_idx.begin(), sort_idx.end(),
            [](const Tuple& a, const Tuple& b) {
              const uint64_t& ca = std::get<0>(a);
              const uint64_t& cb = std::get<0>(b);
              return (ca == cb) ? (std::get<1>(a) < std::get<1>(b)) : (ca > cb);
            });

  // convert Stack* to StackFrames
  result.resize(sort_idx.size());
  for (size_t i = 0; i < sort_idx.size(); i++) {
    result[i].count = std::get<0>(sort_idx[i]);
    const auto* sf = std::get<1>(sort_idx[i]);
    result[i].score = std::get<2>(sort_idx[i]);
    Resolve(sf->addrs, result[i].frames);
  }
}

void Tracker::Resolve(const FramePointers& addr, std::vector<Frame*>& frames) {
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
      addr_lookup[i] = (void*)addr[not_found[i]];
    }
    // @ref https://linux.die.net/man/3/backtrace_symbols
    // current address to symbol, internal malloc-ed
    char** symbols = backtrace_symbols(addr_lookup, num_lookup);
    if (symbols) {
      for (size_t i = 0; i < num_lookup; i++) {
        void* a = addr_lookup[i];
        auto r = all_frames_.emplace(a, resolve_symbol(a, symbols[i]));
        frames[not_found[i]] = &(r.first->second);
      }
      free(symbols);
    }
  }

  // check all set
  for (auto* frame : frames) {
    assert(frame != nullptr);
  }
}

void StackFrameToString(std::ostringstream& oss, const StackFrames& stack,
                        double sum, double sum_score, bool print_symbol) {
  oss << "recorded " << stack.count << " times (" << (stack.count / sum * 100.0)
      << "%), score " << stack.score << " ("
      << (stack.score / sum_score * 100.0) << "%), stack:" << std::endl;
  for (size_t f = 0; f < stack.frames.size(); f++) {
    auto* frame = stack.frames[f];
    oss << "#" << f << (f < 10 ? "  " : " ") << frame->func;
    oss << " at " << frame->file << ":";
    if (frame->line >= 0) {
      oss << frame->line;
    } else {
      oss << "?";
    }
    if (frame->faddr) {
      void* offset = (void*)((char*)frame->addr - (char*)frame->faddr);
      oss << " (" << frame->exec << "+" << offset << ")";
    } else {
      oss << " (" << frame->exec << "+?)";
    }
    if (print_symbol) {
      oss << " <symbol=" << frame->symbol << ">";
    }
    oss << std::endl;
  }
}

std::string StackFramesToString(const std::vector<StackFrames>& records,
                                bool print_symbol) {
  if (records.empty()) {
    return "Report: no records.";
  }
  uint64_t sum = 0;
  int64_t sum_score = 0;
  for (const auto& it : records) {
    sum += it.count;
    sum_score += it.score;
  }
  std::ostringstream oss;
  oss << "Stack format: #N func at file:line (exec+offset)";
  if (print_symbol) {
    oss << " <symbol=...>";
  }
  oss << std::endl
      << "Report: total " << sum << " records, score " << sum_score << ", in "
      << records.size() << " different stack frames:" << std::endl;
  for (size_t i = 0; i < records.size(); i++) {
    const auto& it = records[i];
    oss << "[" << i << "] ";
    StackFrameToString(oss, it, (double)sum, (double)sum_score, print_symbol);
    oss << std::endl;
  }
  return oss.str();
}

void StackFrameToJson(std::ostringstream& oss, const StackFrames& stack,
                      int indent) {
  const std::string ind3(3 * indent, ' ');
  const std::string ind4(4 * indent, ' ');
  if (indent > 0) {
    oss << "{" << std::endl
        << ind3 << "\"count\": " << stack.count << "," << std::endl
        << ind3 << "\"score\": " << stack.score << "," << std::endl
        << ind3 << "\"frames\": [";
  } else {
    oss << "{\"count\": " << stack.count << ", \"score\": " << stack.score
        << ", \"frames\": [";
  }
  for (size_t f = 0; f < stack.frames.size(); f++) {
    auto* frame = stack.frames[f];
    void* offset =
        frame->faddr ? (void*)((char*)frame->addr - (char*)frame->faddr) : 0;
    if (indent > 0) {
      oss << std::endl << ind4;
    }
    oss << "{\"address\": " << (uintptr_t)frame->addr << ", \"function\": \""
        << frame->func << "\", \"file\": \"" << frame->file
        << "\", \"line\": " << frame->line << ", \"exec\": \"" << frame->exec
        << "\", \"offset\": " << (uintptr_t)offset << ", \"symbol\": \""
        << frame->symbol << "\"}";
    if (f < stack.frames.size() - 1) {
      oss << ", ";
    }
  }
  if (indent > 0) {
    oss << std::endl
        << ind3 << "]" << std::endl
        << std::string(2 * indent, ' ') << "}";
  } else {
    oss << "]}";
  }
}

std::string StackFramesToJson(const std::vector<StackFrames>& records,
                              int indent) {
  if (records.empty()) {
    return "{\"sum\": 0, \"sum_score\": 0, \"records\": []}";
  }
  uint64_t sum = 0;
  int64_t sum_score = 0;
  for (const auto& it : records) {
    sum += it.count;
    sum_score += it.score;
  }
  std::ostringstream oss;
  if (indent > 0) {
    const std::string ind(indent, ' ');
    oss << "{" << std::endl
        << ind << "\"sum\": " << sum << "," << std::endl
        << ind << "\"sum_score\": " << sum_score << "," << std::endl
        << ind << "\"records\": [";
  } else {
    oss << "{\"sum\": " << sum << ", \"sum_score\": " << sum_score
        << ", \"records\": [";
  }
  for (size_t i = 0; i < records.size(); i++) {
    const auto& it = records[i];
    if (indent > 0) {
      oss << std::endl << std::string(2 * indent, ' ');
    }
    StackFrameToJson(oss, it, indent);
    if (i < records.size() - 1) {
      oss << ",";
    }
  }
  if (indent > 0) {
    oss << std::endl << std::string(indent, ' ') << "]" << std::endl << "}";
  } else {
    oss << "]}";
  }
  return oss.str();
}

}  // namespace bttrack
