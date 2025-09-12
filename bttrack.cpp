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

void __attribute__((optimize("O1"))) Record(uint8_t id) {
  // use O2/O3 will break Tracker::kSkipFrames
  GetInstance(id).Record();
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
  const size_t cmd_len = frame.exec.size() + 128;
  char cmd[cmd_len];
  // addr2line -e <exec> [-i] <offset>
  snprintf(cmd, cmd_len - 1, "addr2line -e %s %s %p", frame.exec.c_str(),
           (unwind_inline ? "-i" : ""), offset);
  FILE* pipe = popen(cmd, "r");
  if (!pipe) {
    return;
  }
  char buffer[256];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  int status = pclose(pipe);
  int code = WEXITSTATUS(status);
  if (code != 0 || result.empty()) {
    fprintf(stderr, "addr2line failed: returns %d output %s", code,
            result.c_str());
    return;
  }

  // result: "file:line\n", may returns multiple lines if code is optimized
  bool parse_ok = false;
  size_t eol = result.find('\n');  // only use the first line
  if (eol == std::string::npos) {
    eol = result.size();
  }
  size_t colon = result.rfind(':', eol);  // last ':'
  if (colon < result.size() - 1) {
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
    frame.file = result.substr(0, colon);
  }

  // printf("popen(\"%s\") -> \"%s\"\n", cmd, result.c_str());
  // printf("lookup_addr2line: %s:%d\n", frame.file.c_str(), frame.line);
}

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
  frame.faddr = nullptr;

  size_t start = std::string::npos;
  size_t end = std::string::npos;
  bool found = find_func_in_symbol(symbol, start, end);
  if (found) {
    // found function name in symbols
    frame.exec.assign(symbol, start - 1);
    frame.symbol = symbol;
    symbol[end] = '\0';  // null-terminate the substr
    int status;
    char* demangled =
        abi::__cxa_demangle(symbol + start, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      frame.func = demangled;
      free(demangled);
    } else {
      frame.func.assign(symbol + start);
    }
  } else if (symbol) {
    // no function name in symbol
    frame.symbol = symbol;
    frame.func = "__unknown__";
  } else {
    frame.symbol = "(nil)";
    frame.func = "__unknown__";
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
      << "%), stack:" << std::endl;
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
  oss << "Stack format: #N func at file:line (exec+offset)";
  if (print_symbol) {
    oss << " <symbol=...>";
  }
  oss << std::endl
      << "Report: total " << sum << " records, in " << records.size()
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