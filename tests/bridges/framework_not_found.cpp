// Stands in for the tests of a bridge whose framework was not found when the
// build was configured, so that the test report lists them as skipped instead
// of leaving them out without a trace. Prints its arguments (the reason).
#include <cstddef>
#include <iostream>
#include <span>

namespace {

// ctest reports a test that exits with its SKIP_RETURN_CODE as skipped.
constexpr int skip_return_code = 77;

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    for (const char* arg : args.subspan(1)) {
        std::cout << arg << ' ';
    }
    std::cout << '\n';
    return skip_return_code;
}
