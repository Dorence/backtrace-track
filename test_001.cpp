#include <unistd.h>

#include <cstdio>

#include "bttrack.h"

using namespace bttrack;

void RunCall() { Record(0); }

void RunCall2(int any) {
  Record(0);
  RunCall();
  Record(0);
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
    Record(0);  // record 100 times
  }
  RunCall();                  // record 1 time
  int sum = RunCall3<int>();  // record 20+40 times
  printf("sum = %d\n", sum);
}

void Print(int id) {
  std::vector<StackFrames> records;
  Dump(id, records);
  bool print_symbol = true;
  std::string report = PrintStackFrames(records, print_symbol);
  printf("%s\n", report.c_str());
}

int main() {
  Run();
  Print(0);
  return 0;
}
