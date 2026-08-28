#pragma once

#include <Sora/Platform.h>

namespace Sora {

    namespace Concurrency {

        namespace Exec {

            enum class Disposition : uint8_t { Value, Error, Stopped };

            template<Disposition D>
            struct DispositionTag {
                static inline constexpr Disposition disposition = D;

                template<Disposition Other>
                [[nodiscard]] constexpr auto operator==(DispositionTag<Other>) const noexcept -> bool {
                    return disposition == Other;
                }
            };

            namespace Concept {

                template<typename T>
                concept DispositionTagType = requires {
                    { T::disposition } -> std::same_as<Disposition>;
                } && std::same_as<T, DispositionTag<T::disposition>>;

                template<typename R, typename... Args>
                concept HasSetValue = requires(R&& receiver, Args&&... args) {
                    { static_cast<R &&>(receiver).set_value(std::forward<Args>(args)...) } -> std::same_as<void>;
                };

            } // namespace Concept

        } // namespace Exec

    } // namespace Concurrency

} // namespace Sora