#include "../src/AddressLibrary.h"
#include <cassert>
#include <sstream>
#include <vector>
#include <iostream>

std::string Bin(std::vector<std::uint64_t> words)
{
    return {reinterpret_cast<const char*>(words.data()), words.size() * sizeof(words[0])};
}
void Check(const std::string& data, std::uint64_t id, std::optional<std::uint64_t> expected)
{
    std::istringstream input(data, std::ios::binary);
    assert(Addr::ExactOffset(input, id) == expected);
}
int main()
{
    const auto valid = Bin({3, 10, 100, 20, 200, 30, 300});
    Check(valid, 10, 100); Check(valid, 20, 200); Check(valid, 30, 300);
    Check(valid, 0, {}); Check(valid, 15, {}); Check(valid, 31, {});
    Check("", 10, {}); Check(valid.substr(0, valid.size() - 1), 10, {});
    Check(valid + 'x', 10, {}); Check(Bin({~0ull, 10, 100}), 10, {});
    Check(Bin({0}), 10, {}); Check(Bin({1, 10, 0}), 10, {});
    std::cout << "Address Library exact-ID and malformed-file cases passed\n";
}
