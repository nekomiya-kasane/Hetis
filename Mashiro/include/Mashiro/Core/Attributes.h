/**
 * @file Attributes.h
 * @brief Portable compiler-attribute spellings used by Mashiro headers.
 * @ingroup Core
 */
#pragma once

#if defined(_MSC_VER)
#    define MASHIRO_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#    define MASHIRO_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
