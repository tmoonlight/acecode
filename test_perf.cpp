#include <iostream>
#include <filesystem>
#include <chrono>

int main() {
    auto start = std::chrono::high_resolution_clock::now();
    std::error_code ec;
    auto w = std::filesystem::weakly_canonical("N:\\Users\\shao\\acedesktoptest", ec);
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "weakly_canonical: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms\n";
    return 0;
}
