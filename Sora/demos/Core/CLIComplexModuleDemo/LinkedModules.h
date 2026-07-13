/**
 * @file LinkedModules.h
 * @brief Linked startup-fragment entry points used by the complex CLI module demo.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/CLI/Fragment.h>
#include <Sora/Platform.h>

extern "C" PLATFORM_IMPORT const Sora::CLI::CommandFragment* SoraCliComplexLinkedA() noexcept;
