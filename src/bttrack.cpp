#include "bttrack.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "ipp_inc.ipp"

namespace bttrack {

const char* kFuncUnknown = "<unknown>";

#include "slice.ipp"
#include "utils.ipp"

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

// @ref https://sourceware.org/binutils/docs-2.31/binutils/addr2line.html
class Addr2lineTool {
 public:
  struct Context {
    /**
     * [in-out] changeable
     * - frames must not be empty
     * - frames[]->exec must be the same
     */
    std::vector<Frame*> frames;
    bool display_func;    // [in]
    bool unwind_inline;   // [in]
    int ret;              // [out] return code, 0 if success
    std::string err_msg;  // error message if ret != 0
  };

  static Addr2lineTool* GetInstance() {
    static Addr2lineTool instance;
    return &instance;  // singleton
  }

  static bool is_addr2line_available() {
    return GetInstance()->is_addr2line_available_;
  }

  void Resolve(std::vector<Frame*>& frames) {
    assert(!frames.empty());
    auto start = get_nanos();
    Context ctx;
    std::vector<bool> resolved(frames.size(), false);
    size_t i = 0;
    while (i < frames.size()) {
      if (resolved[i]) {
        i++;
        continue;
      }
      // reset states
      ctx.frames.clear();
      ctx.unwind_inline = true;              // @todo dynamic option
      ctx.display_func = ctx.unwind_inline;  // should get inline func name
      // find all frames with the same exec (no more than batch_size)
      resolved[i] = true;
      ctx.frames.push_back(frames[i]);
      const auto& exec = ctx.frames[0]->exec;
      int batch_size = 100;
      for (size_t j = i + 1; j < frames.size(); j++) {
        if (frames[j]->exec == exec) {
          ctx.frames.push_back(frames[j]);
          resolved[j] = true;
          if (!ctx.display_func && frames[j]->func != kFuncUnknown) {
            ctx.display_func = true;
          }
          if (--batch_size == 0) {
            break;
          }
        }
      }
      // lookup addr2line
      BatchResolve(ctx);
      i++;
    }
    double elapsed = (get_nanos() - start) / 1e9;
    if (elapsed > 1.0) {
      fprintf(stderr, "Addr2LineTool::Resolve %lu in %.3fs\n", frames.size(),
              elapsed);
    }
  }

  // all frames should have the same exec path
  void BatchResolve(Context& ctx) {
    assert(!ctx.frames.empty());
    auto& exec = ctx.frames[0]->exec;
    std::string cmd = "addr2line -e " + exec + " -p";
    if (ctx.display_func) {
      cmd += " -f";
    }
    if (ctx.unwind_inline) {
      cmd += " -i";
    }
    const auto& base = ctx.frames[0]->faddr;
    for (auto frame : ctx.frames) {
      cmd += " ";
      assert(frame->faddr == base);
      cmd += to_address(Slice::offset(frame->addr, base));
    }
    // fprintf(stderr, " >> cmd: %s\n", cmd.c_str());
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      ctx.ret = -1;
      ctx.err_msg = "popen addr2line failed";
      return;
    }
    char buffer[4096];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      result += buffer;
    }
    int status = pclose(pipe);
    ctx.ret = WEXITSTATUS(status);
    if (ctx.ret) {
      ctx.err_msg = "addr2line failed";
      result.clear();
    }
    // fprintf(stderr, " >> out: \"%s\"\n", result.c_str());
    ParseBatch(ctx, result);
  }

 private:
  const bool is_addr2line_available_;

  Addr2lineTool() : is_addr2line_available_(FindAddr2line()) {}

  // find addr2line using which or whereis
  static bool FindAddr2line() {
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
      // fprintf(stderr, "addr2line found: %s", result.c_str());
    }
    return ok;
  }

  void ParseBatch(Context& ctx, const std::string& result) {
    if (result.empty()) {
      return;
    }
    Slice data(result);
    int frame_id = -1;
    while (frame_id < static_cast<int>(ctx.frames.size()) && !data.empty()) {
      // find line and shift data
      Slice line = data.substr(0, data.find('\n'));
      if (line.empty()) {
        assert(false);
        break;
      }
      data.pop_front(line.size() + 1);
      // printf("ParseLine \"%s\", remains %lu\n", line.to_string().c_str(),
      //        data.size());
      ParseLine(ctx, frame_id, line);
    }
  }

  /**
   * always with `-p`, three possible formats:
   * 1. ` `: no func, no inline
   *   format "FILE:LINE\n" (1 line), may be trailing with `(discriminator 2)`
   * 2. `-f`: print func, no inline
   *   format "FUNC at FILE:LINE\n" (1 line)
   * 3. `-i`: unwind inline, no func (deprecated)
   *   - "FILE:LINE\n" (1 line)
   *   - "FILE:LINE\n (inlined by) FILE:LINE\n..." (1+n lines)
   * 4. `-f -i`: print func, unwind inline
   *   - "FUNC at FILE:LINE\n" (1 line)
   *   - "FUNC at FILE:LINE\n (inlined by) FUNC at FILE:LINE\n..." (1+n lines)
   */
  void ParseLine(Context& ctx, int& frame_id, Slice& line) {
    static const Slice kInlinedBy(" (inlined by) ", 14);
    static const Slice kAt(" at ", 4);

    // (optional) parse " (inlined by) "
    bool is_inlined_by = line.starts_with(kInlinedBy);
    if (is_inlined_by) {
      line.pop_front(kInlinedBy.size());
    } else {
      frame_id++;  // a new frame
    }
    assert(frame_id >= 0);  // starts from -1
    Frame* frame = ctx.frames[frame_id];

    // (optional) parse "FUNC at "
    Slice func;
    std::string func_str;
    size_t pos_at = line.find(kAt);
    if (pos_at != Slice::npos) {
      func = line.substr(0, pos_at);
      line.pop_front(pos_at + kAt.size());
      demangle_symbol(func_str, func.make_cstr());
    }

    // parse "FILE:LINE"
    size_t pos_colon = line.find(':');
    if (pos_colon == Slice::npos) {
      fprintf(stderr, "ParseLine: no colon found. ERROR!!!");
      return;
    }
    size_t len_file = pos_colon;
    size_t len_line = line.size() - pos_colon - 1;
    if (len_file == 0 || len_line == 0) {
      fprintf(stderr, "ParseLine: file or line length is 0. ERROR!!!");
      return;
    }
    std::string file = line.substr(0, len_file).to_string();
    int line_no = ParseLineNumber(line.substr(pos_colon + 1, len_line));

    if (is_inlined_by) {
      Frame::Func f{
          .name = func.empty() ? kFuncUnknown : std::move(func_str),
          .file = std::move(file),
          .line = line_no,
      };
      frame->inlined_by.emplace_back(f);
      // printf("  [inline] %s at %s:%d\n", f.name.c_str(), f.file.c_str(),
      //        line_no);
    } else {
      frame->file = std::move(file);
      frame->line = line_no;
      if (!func_str.empty() && frame->func != func_str) {
        // always overwrite because func_str may be inline function
        frame->func = std::move(func_str);
      }
      // printf("  [new] file '%s' line %d func %s\n", frame->file.c_str(),
      //        frame->line, func.empty() ? "(null)" : frame->func.c_str());
    }
  }

  int ParseLineNumber(const Slice& s) noexcept {
    int ret = -1;
    if (s[0] == '?') {
      return ret;
    }
    try {
      ret = std::stoi(s.to_string());
    } catch (...) {
      // ignore parse error
    }
    return ret;
  }

  std::string to_address(uintptr_t addr) {
    char buf[36];  // 64b "0xFEDCBA9876543210" -> 18, 128b -> 34
    snprintf(buf, sizeof(buf), "%p", (void*)addr);
    return std::string(buf);
  }
};

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
    assert(start != std::string::npos);
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

  // later get source code using addr2line
  frame.file = "??";
  frame.line = -1;

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
      frames[i] = nullptr;
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
      std::vector<Frame*> to_call_addr2line;
      to_call_addr2line.reserve(num_lookup);
      for (size_t i = 0; i < num_lookup; i++) {
        void* a = addr_lookup[i];
        auto r = all_frames_.emplace(a, resolve_symbol(a, symbols[i]));
        Frame* f = &(r.first->second);
        frames[not_found[i]] = f;
        to_call_addr2line.emplace_back(f);
      }
      free(symbols);

      // call addr2line
      Addr2lineTool::GetInstance()->Resolve(to_call_addr2line);
    } else {
      fprintf(stderr, "backtrace_symbols() call failed\n");
    }
  }

  // check all set
  for (auto* frame : frames) {
    assert(frame != nullptr);
  }
}

#include "output.ipp"

}  // namespace bttrack
