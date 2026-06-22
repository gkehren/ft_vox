#include <iostream>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

void mockRenderTextString(std::string text) {
    volatile int dummy = 0;
    for(char c : text) {
        dummy = dummy + c;
    }
}

void mockRenderTextStringView(std::string_view text) {
    volatile int dummy = 0;
    for(char c : text) {
        dummy = dummy + c;
    }
}

int main() {
    std::string longText = "This is a reasonably long string that is definitely longer than 15 characters to bypass the Small String Optimization (SSO) in most modern C++ standard libraries. We want to make sure it allocates.";

    const int numIterations = 10000000;

    auto start1 = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < numIterations; ++i) {
        mockRenderTextString(longText);
    }
    auto end1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> diff1 = end1 - start1;

    auto start2 = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < numIterations; ++i) {
        mockRenderTextStringView(longText);
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> diff2 = end2 - start2;

    std::cout << "std::string by value: " << diff1.count() << " ms\n";
    std::cout << "std::string_view: " << diff2.count() << " ms\n";

    return 0;
}
