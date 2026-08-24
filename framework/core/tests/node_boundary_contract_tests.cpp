// SPDX-License-Identifier: Apache-2.0

#include "../src/bindings/node/binding/byte_view.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using kungfu::node::boundary::byte_view_error;
using kungfu::node::boundary::copy_exact;
using kungfu::node::boundary::replace_vector;

#define CHECK(expression)                                                                                              \
  do {                                                                                                                 \
    if (!(expression)) {                                                                                               \
      return __LINE__;                                                                                                 \
    }                                                                                                                  \
  } while (false)

int main() {
  const std::array<std::uint32_t, 2> source{0x01020304u, 0xaabbccddu};
  std::uint32_t fixed[2]{};
  CHECK(copy_exact(source.data(), sizeof(source), fixed) == byte_view_error::none);
  CHECK(fixed[0] == source[0]);
  CHECK(fixed[1] == source[1]);
  CHECK(copy_exact(source.data(), sizeof(source) - 1, fixed) == byte_view_error::size_mismatch);
  CHECK(copy_exact(nullptr, sizeof(source), fixed) == byte_view_error::null_data);

  std::array<std::byte, sizeof(source) + 1> misaligned{};
  std::memcpy(misaligned.data() + 1, source.data(), sizeof(source));
  std::vector<std::uint32_t> values;
  CHECK(replace_vector(misaligned.data() + 1, sizeof(source), values) == byte_view_error::none);
  CHECK(values.size() == source.size());
  CHECK(values[0] == source[0]);
  CHECK(values[1] == source[1]);
  CHECK(replace_vector(misaligned.data() + 1, sizeof(source) - 1, values) == byte_view_error::size_not_multiple);
  CHECK(replace_vector(nullptr, sizeof(source), values) == byte_view_error::null_data);
}
