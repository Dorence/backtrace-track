#include "ipp_inc.h"

// a simple stringview
class Slice {
 public:
  using size_t = std::size_t;
  static const size_t npos = std::string::npos;  // means not found

  Slice() : data_(nullptr), size_(0) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s, size_t n) : data_(s), size_(n) {}

  bool empty() const { return size_ == 0 || data_ == nullptr; }

  size_t find(char c) const {
    if (empty()) return npos;
    auto* p = ::memchr(data_, c, size_);  // match char
    return p ? offset(p, data_) : npos;
  }
  size_t find(const char* s, size_t n = npos) const {
    if (n == npos) n = ::strlen(s);
    if (empty() || n == 0 || n > size_) return npos;
    auto* p = ::memmem(data_, size_, s, n);  // match n bytes
    return p ? offset(p, data_) : npos;
  }
  size_t find(const Slice& s) const { return find(s.data_, s.size_); }
  size_t find(const std::string& s) const { return find(s.data(), s.size()); }

  void pop_front(size_t n = 1) {
    if (n >= size_) {
      data_ = nullptr;
      size_ = 0;
    } else {
      data_ += n;
      size_ -= n;
    }
  }

  size_t size() const { return size_; }

  bool starts_with(const Slice& s) const {
    if (empty()) return false;
    return size_ >= s.size_ && ::memcmp(data_, s.data_, s.size_) == 0;
  }

  Slice substr(size_t pos = 0, size_t n = npos) const {
    if (empty() || pos >= size_) return Slice(data_, 0);
    return Slice(data_ + pos, std::min(n, size_ - pos));
  }

  std::string to_string() const { return std::string(data_, size_); }

  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }

  // [Dangerous] You must know what you are doing!
  const char* make_cstr() {
    if (empty()) return nullptr;
    const_cast<char*>(data_)[size_] = '\0';
    return data_;
  }

  template <typename T1, typename T2>
  static size_t offset(T1 a, T2 b) {
    return (uintptr_t)a - (uintptr_t)b;
  }

 protected:
  const char* data_;
  size_t size_;
};