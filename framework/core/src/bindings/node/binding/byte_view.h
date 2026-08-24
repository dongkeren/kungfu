// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_NODE_BYTE_VIEW_H
#define KUNGFU_NODE_BYTE_VIEW_H

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace kungfu::node::boundary {

enum class byte_view_error { none, null_data, size_mismatch, size_not_multiple };

template <typename ValueType, std::size_t Length>
byte_view_error copy_exact(const void *data, std::size_t byte_length, ValueType (&destination)[Length]) {
  static_assert(std::is_trivially_copyable_v<ValueType>);
  constexpr auto expected = sizeof(destination);
  if (byte_length != expected) {
    return byte_view_error::size_mismatch;
  }
  if (byte_length != 0 && data == nullptr) {
    return byte_view_error::null_data;
  }
  std::memcpy(destination, data, expected);
  return byte_view_error::none;
}

template <typename ValueType>
byte_view_error replace_vector(const void *data, std::size_t byte_length, std::vector<ValueType> &destination) {
  static_assert(std::is_trivially_copyable_v<ValueType>);
  if (byte_length % sizeof(ValueType) != 0) {
    return byte_view_error::size_not_multiple;
  }
  if (byte_length != 0 && data == nullptr) {
    return byte_view_error::null_data;
  }
  destination.resize(byte_length / sizeof(ValueType));
  if (byte_length != 0) {
    // std::vector owns correctly aligned storage. Copying bytes into it avoids
    // dereferencing a potentially misaligned JavaScript ArrayBuffer view.
    std::memcpy(destination.data(), data, byte_length);
  }
  return byte_view_error::none;
}

} // namespace kungfu::node::boundary

#endif // KUNGFU_NODE_BYTE_VIEW_H
