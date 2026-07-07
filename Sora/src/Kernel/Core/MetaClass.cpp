/**
 * @file MetaClass.cpp
 * @brief Canonical runtime metaclass registry and provider lookup.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/MetaClass.h"

#include <cassert>
#include <mutex>

namespace Sora::Kernel {

    namespace {

        std::mutex& MetaClassRegistryMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<Iid, std::shared_ptr<MetaClass>>& MetaClassRegistry() {
            static std::unordered_map<Iid, std::shared_ptr<MetaClass>> registry;
            return registry;
        }

    } // namespace

    std::shared_ptr<MetaClass> MetaClass::Intern(std::shared_ptr<MetaClass> meta) noexcept {
        if (!meta || IsNil(meta->iid)) {
            return meta;
        }

        std::scoped_lock lock(MetaClassRegistryMutex());
        auto& registry = MetaClassRegistry();
        if (auto found = registry.find(meta->iid); found != registry.end()) {
            assert(found->second->type == meta->type);
            return found->second;
        }

        registry.emplace(meta->iid, meta);
        return meta;
    }

    const ProviderEntry* MetaClass::FindProvide(Iid iid) const noexcept {
        for (const MetaClass* current = this; current != nullptr; current = current->base.get()) {
            if (auto found = current->provides.find(iid); found != current->provides.end()) {
                return std::addressof(found->second);
            }
        }
        return nullptr;
    }

} // namespace Sora::Kernel
