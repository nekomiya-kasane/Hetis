#pragma once
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <meta>
#include <type_traits>

#define Y_OBJECT                                                                                    \
  public:                                                                                           \
    using SelfType = typename [: ::std::meta::access_context::current().scope() :];                 \
                                                                                                    \
    static constexpr ::Yuki::MetaCore kMetaCore = ::Yuki::Detail::MakeMetaCoreFor<SelfType>();      \
    static const ::Yuki::MetaDynamic& Meta() noexcept {                                             \
        return ::Yuki::MetaDynamicOf<SelfType>;                                                     \
    }                                                                                               \
    virtual const ::Yuki::MetaDynamic& MetaDyn() const noexcept override {                          \
        return ::Yuki::MetaDynamicOf<SelfType>;                                                     \
    }                                                                                               \
    friend struct ::Yuki::Detail::MetaHook<SelfType>;                                               \
                                                                                                    \
  private:                                                                                          \
    [[no_unique_address, msvc::no_unique_address]] ::Yuki::Detail::MetaHook<SelfType> _yukiHook_{}; \
                                                                                                    \
  public:
