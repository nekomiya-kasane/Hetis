#pragma once
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>
#include <meta>
#include <type_traits>

#define Y_OBJECT                                                                       \
  public:                                                                              \
    using YukiSelf =                                                                   \
        typename [: ::std::meta::access_context::current().scope() :];                 \
    static constexpr ::Yuki::MetaCore kMetaCore =                                      \
        ::Yuki::Detail::MakeMetaCoreFor<YukiSelf>();                                   \
    friend struct ::Yuki::Detail::MetaHook<YukiSelf>;                                  \
  private:                                                                             \
    [[no_unique_address, msvc::no_unique_address]]                                     \
    ::Yuki::Detail::MetaHook<YukiSelf> _yukiHook_{};                                   \
  public:
