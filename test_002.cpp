#include "bttrack.h"

class JobFoo {
 public:
  void PrepareJob() {
    bool ok = bttrack::GetBacktrace(prev_stack_);
    prev_score_ = ok ? (-2 * id) : 0;
  }

  void RunJob() {
    auto score = rand() % 0xfffff;  // 0-1M
    if (score > 0x7ffff) {
      bttrack::Record(0, score);
      bttrack::Record(1, prev_stack_, prev_score_);  // 50%
    } else {
      bttrack::Record(0, score);
    }
  }

  int id = 0;

 private:
  bttrack::FramePointers prev_stack_;
  int64_t prev_score_ = 0;
};

void Run() {
  JobFoo job[8];
  for (int i = 0; i < 8; i++) {
    job[i].id = i + 1;
  }

  const int kNumRuns = 1000;
  for (int i = 0; i < kNumRuns; i++) {
    size_t idx = rand() % 8;
    job[idx].PrepareJob();
    job[idx].RunJob();
  }

  // output
  std::vector<bttrack::StackFrames> records;
  std::string json;
  const int json_indent = 2;

  bttrack::Dump(0, records);
  json = bttrack::StackFramesToJson(records, json_indent);
  printf("Channel 0 JSON:\n%s\n\n", json.c_str());

  bttrack::Dump(1, records);
  json = bttrack::StackFramesToJson(records, json_indent);
  // count(n) is about 50% of kNumRuns
  printf("Channel 1 JSON:\n%s\n\n", json.c_str());
}

int main() {
  srand(time(NULL));
  Run();
  return 0;
}
