#pragma once
#include <Yuki/Core/ClassType.h>
#include <Yuki/Core/TaggedPayload.h>
#include <atomic>

namespace Yuki {
    struct RootObject {
        explicit RootObject(ClassType role, void* arm, bool external) noexcept
          : metaWord_(external
                ? TaggedPayload::MakeExternal(role, arm)
                : TaggedPayload::Make(role, arm, /*rc=*/1, /*ever=*/true))
        {}
        virtual ~RootObject() noexcept;

        RootObject(const RootObject&) = delete;
        RootObject& operator=(const RootObject&) = delete;

        ClassType TypeDynamic() const noexcept {
            return metaWord_.load(std::memory_order_relaxed).role();
        }
        TaggedPayload PayloadRelaxed() const noexcept {
            return metaWord_.load(std::memory_order_relaxed);
        }

        // Acquire/Release defined in Task 12.
        std::atomic<TaggedPayload>& MetaWord() noexcept { return metaWord_; }

      private:
        std::atomic<TaggedPayload> metaWord_;
    };
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));
}
