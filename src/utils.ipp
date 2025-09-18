static uint64_t get_nanos() {
  struct timespec ts;
  constexpr uint64_t kNanosInSec = 1000 * 1000 * 1000;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * kNanosInSec + ts.tv_nsec;
}