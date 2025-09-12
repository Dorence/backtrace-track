#include <unistd.h>

#include <cstdio>

#include "bttrack.h"

void RunCall() { bttrack::Record(0); }

void RunCall2(int any) {
  bttrack::Record(0, 100);
  RunCall();
  bttrack::Record(0);
}

template <typename T>
T RunCall3() {
  T sum = 0;
  for (T i = 0; i < 20; i++) {
    ::usleep(1);
    RunCall2(i);
    sum += i;
  }
  return sum;
}

void Run() {
  for (int i = 0; i < 100; i++) {
    bttrack::Record(0);  // record 100 times
  }
  RunCall();                  // record 1 time
  int sum = RunCall3<int>();  // record 20+40 times
  printf("sum = %d\n", sum);
}

void Print(int id) {
  bool print_symbol = true;
  int json_indent = 2;

  std::vector<bttrack::StackFrames> records;
  bttrack::Dump(id, records);
  std::string report = bttrack::StackFramesToString(records, print_symbol);
  printf("%s", report.c_str());
  report = bttrack::StackFramesToJson(records, json_indent);
  printf("JSON:\n%s\n", report.c_str());
}

int main() {
  Run();
  Print(0);
  return 0;
}
