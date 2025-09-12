#include <cstdio>

#include "bttrack.h"

using namespace bttrack;

void RunCall() { Record(0); }

void RunCall2() {
  Record(0);
  RunCall();
  Record(0);
}

void RunCall3() {
  for (int i = 0; i < 20; i++) {
    RunCall2();
  }
}

void Run() {
  for (int i = 0; i < 100; i++) {
    Record(0);  // record 100 times
  }
  RunCall();   // record 1 time
  RunCall3();  // record 20+40 times
}

void Print(int id) {
  std::vector<StackFrames> records;
  Dump(id, records);
  bool print_symbol = false;
  std::string report = PrintStackFrames(records, print_symbol);
  printf("%s\n", report.c_str());
}

int main() {
  Run();
  Print(0);
  return 0;
}
