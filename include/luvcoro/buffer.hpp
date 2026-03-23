#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace luvcoro {

class mutable_buffer {
 public:
  constexpr mutable_buffer() noexcept = default;
  constexpr mutable_buffer(void* data, std::size_t size) noexcept
      : data_(static_cast<char*>(data)), size_(size) {}

  constexpr auto data() const noexcept -> char* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  constexpr auto subspan(std::size_t offset) const noexcept -> mutable_buffer {
    if (offset >= size_) {
      return {};
    }
    return mutable_buffer(data_ + offset, size_ - offset);
  }

  constexpr auto as_span() const noexcept -> std::span<char> {
    return std::span<char>(data_, size_);
  }

 private:
  char* data_ = nullptr;
  std::size_t size_ = 0;
};

class const_buffer {
 public:
  constexpr const_buffer() noexcept = default;
  constexpr const_buffer(const void* data, std::size_t size) noexcept
      : data_(static_cast<const char*>(data)), size_(size) {}

  constexpr auto data() const noexcept -> const char* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  constexpr auto subspan(std::size_t offset) const noexcept -> const_buffer {
    if (offset >= size_) {
      return {};
    }
    return const_buffer(data_ + offset, size_ - offset);
  }

  constexpr auto as_span() const noexcept -> std::span<const char> {
    return std::span<const char>(data_, size_);
  }

 private:
  const char* data_ = nullptr;
  std::size_t size_ = 0;
};

inline constexpr auto buffer(void* data, std::size_t size) noexcept -> mutable_buffer {
  return mutable_buffer(data, size);
}

inline constexpr auto buffer(const void* data, std::size_t size) noexcept -> const_buffer {
  return const_buffer(data, size);
}

inline constexpr auto buffer(char* data, std::size_t size) noexcept -> mutable_buffer {
  return mutable_buffer(data, size);
}

inline constexpr auto buffer(const char* data, std::size_t size) noexcept -> const_buffer {
  return const_buffer(data, size);
}

template <std::size_t N>
inline constexpr auto buffer(char (&data)[N]) noexcept -> mutable_buffer {
  return mutable_buffer(data, N);
}

template <std::size_t N>
inline constexpr auto buffer(const char (&data)[N]) noexcept -> const_buffer {
  return const_buffer(data, N);
}

template <typename T, std::size_t N>
inline constexpr auto buffer(std::array<T, N>& data) noexcept -> mutable_buffer
  requires(std::is_same_v<T, char>)
{
  return mutable_buffer(data.data(), data.size());
}

template <typename T, std::size_t N>
inline constexpr auto buffer(const std::array<T, N>& data) noexcept -> const_buffer
  requires(std::is_same_v<T, char>)
{
  return const_buffer(data.data(), data.size());
}

inline auto buffer(std::vector<char>& data) noexcept -> mutable_buffer {
  return mutable_buffer(data.data(), data.size());
}

inline auto buffer(const std::vector<char>& data) noexcept -> const_buffer {
  return const_buffer(data.data(), data.size());
}

inline auto buffer(std::string& data) noexcept -> mutable_buffer {
  return mutable_buffer(data.data(), data.size());
}

inline auto buffer(const std::string& data) noexcept -> const_buffer {
  return const_buffer(data.data(), data.size());
}

inline auto buffer(std::string_view data) noexcept -> const_buffer {
  return const_buffer(data.data(), data.size());
}

inline constexpr auto buffer(std::span<char> data) noexcept -> mutable_buffer {
  return mutable_buffer(data.data(), data.size());
}

inline constexpr auto buffer(std::span<const char> data) noexcept -> const_buffer {
  return const_buffer(data.data(), data.size());
}

}  // namespace luvcoro
