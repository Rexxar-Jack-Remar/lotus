#pragma once

#include <spdlog/fmt/fmt.h>

#include <llvm/Support/raw_ostream.h>

#include <string>
#include <type_traits>
#include <utility>

#if !defined(SPDLOG_USE_STD_FORMAT)
namespace lotus {
namespace logging {
namespace detail {

template <typename T, typename = void>
struct IsRawOstreamWritable : std::false_type {};

template <typename T>
struct IsRawOstreamWritable<
    T, std::void_t<decltype(std::declval<llvm::raw_ostream &>()
                            << std::declval<const T &>())>> : std::true_type {};

} // namespace detail
} // namespace logging
} // namespace lotus

namespace fmt {

/// Preserve the logging behavior Lotus had with its old bundled fmt: custom
/// LLVM and analysis objects that implement raw_ostream insertion can be used
/// directly in spdlog format arguments.
template <typename T>
struct formatter<
    T, char,
    std::enable_if_t<
        !std::is_convertible<const T &, string_view>::value &&
        detail::type_constant<T, char>::value == detail::type::custom_type &&
        lotus::logging::detail::IsRawOstreamWritable<T>::value>>
    : formatter<string_view> {
  template <typename FormatContext>
  auto format(const T &value, FormatContext &ctx) const
      -> decltype(ctx.out()) {
    std::string buffer;
    llvm::raw_string_ostream stream(buffer);
    stream << value;
    stream.flush();
    return formatter<string_view>::format(buffer, ctx);
  }
};

} // namespace fmt
#endif
