/**
 * @file LinkedModules.h
 * @brief Imported startup-fragment entry points supplied by the linked CLI demo DLLs.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/CLI/Fragment.h>
#include <Sora/Platform.h>

extern "C" PLATFORM_IMPORT const Sora::CLI::CommandFragment* SoraCliLinkedAlpha() noexcept;
extern "C" PLATFORM_IMPORT const Sora::CLI::CommandFragment* SoraCliLinkedBeta() noexcept;
