#include "test_runner.h"

#include <cstdlib>

using namespace picamera::test;

int main() {
  int passed = 0;
  int failed = 0;
  for (const auto &tc : registry()) {
    std::cout << "[ RUN      ] " << tc.name << "\n";
    int checksBefore = failCount();
    try {
      tc.fn();
      if (failCount() > checksBefore) {
        std::cout << "[  FAILED  ] " << tc.name << " (CHECK failures)\n";
        ++failed;
      } else {
        std::cout << "[       OK ] " << tc.name << "\n";
        ++passed;
      }
    } catch (const Failure &) {
      std::cout << "[  FAILED  ] " << tc.name << "\n";
      ++failed;
    } catch (const std::exception &e) {
      std::cerr << "  unexpected exception: " << e.what() << "\n";
      std::cout << "[  FAILED  ] " << tc.name << "\n";
      ++failed;
    }
  }

  std::cout << "\n"
            << passed << " passed, " << failed << " failed"
            << " (" << registry().size() << " total)"
            << ", " << checkFailures() << " failed checks\n";
  return failed == 0 ? 0 : 1;
}
