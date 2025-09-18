#include "ipp_inc.h"

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
    if (!frame->inlined_by.empty()) {
      oss << " (inlined by " << frame->inlined_by.size() << ")";
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
        << frame->symbol << "\", \"inlined_by\": " << frame->inlined_by.size()
        << "}";
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
