#pragma once

#define DeclareError(CLASS_NAME)                                 \
 public:                                                         \
  constexpr static std::string_view Name = #CLASS_NAME;          \
                                                                 \
  inline constexpr virtual std::string_view ClassName() const {  \
    static_assert(CLASS_NAME::Name == Name, "Wrong class name"); \
    return CLASS_NAME::Name;                                     \
  }