/**
 * @file Registry.h
 * @brief Process-wide registrar bookkeeping for the Yuki closure model — spec §3.3 step 3–4.
 *
 * The registrar runs once per class (driven by Task 9's CRTP hook). Before publishing a new
 * snapshot it must:
 *
 *   - Check @ref Yuki::Registry::AlreadyInstalled to make duplicate registrar invocations a
 *     no-op (the same TU can run twice — once in the host exe and once in a plugin DLL whose
 *     class lists overlap).
 *   - Take @ref Yuki::Registry::WriterMutexFor for the metaclass it is about to publish, so two
 *     writers racing on the *same* class serialize while writers on different classes proceed
 *     concurrently. The mutex is per-metaclass (not global) precisely so independent installs
 *     do not fight over a shared lock at startup.
 *
 * The body of @c Install lands in @ref Yuki/Core/Registry.h (Tasks 7–8); this header declares
 * only the bookkeeping primitives. All functions are noexcept — registrars run during static
 * init and cannot throw without taking down the process.
 */
#pragma once

#include <Yuki/Core/MetaClass.h>

#include <mutex>

namespace Yuki {

    namespace Registry {

        /**
         * @brief Has @p id already been registered? Idempotency gate for the registrar.
         *
         * Returns @c true iff a prior @ref MarkInstalled call recorded @p id. Cheap to call (a
         * short mutex-guarded set lookup) — readers may also use it to test "does this class
         * even exist yet" during diagnostic paths.
         */
        [[nodiscard]] bool AlreadyInstalled(Iid id) noexcept;

        /**
         * @brief Record @p id as installed. Idempotent — duplicate calls are no-ops.
         *
         * Called by the registrar after a successful publish so subsequent runs of the same
         * registrar TU short-circuit at @ref AlreadyInstalled.
         */
        void MarkInstalled(Iid id) noexcept;

        /**
         * @brief Hand out the writer mutex for @p mc. Same metaclass → same mutex, every call.
         *
         * Lazily inserts a fresh @c std::mutex into the per-metaclass map on first request.
         * The map uses @c std::map (not @c unordered_map) so the returned reference stays valid
         * across later inserts — node-based maps don't rehash, so the mutex address is stable
         * for the life of the process.
         */
        [[nodiscard]] std::mutex& WriterMutexFor(const MetaClass& mc) noexcept;

    } // namespace Registry

} // namespace Yuki
