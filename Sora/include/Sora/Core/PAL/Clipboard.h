/**
 * @file Clipboard.h
 * @brief Process-global system clipboard text access through engine-wide error codes.
 * @ingroup PAL
 *
 * @details Clipboard text is exposed as UTF-8. An empty optional returned by @ref ReadText means that the clipboard
 * does not currently advertise a native Unicode text format; an engaged empty string is valid text and remains
 * distinct.
 * Clipboard operations may contend with other processes and therefore report @ref ErrorCode::ClipboardBusy.
 */
#pragma once

#include <Sora/ErrorCode.h>

#include <optional>
#include <string>
#include <string_view>

namespace Sora::PAL::Clipboard {

    /** @brief Read the current native Unicode clipboard text as UTF-8. */
    [[nodiscard]] Result<std::optional<std::string>> ReadText();

    /** @brief Replace clipboard contents with @p text after strict UTF-8 validation. */
    [[nodiscard]] VoidResult WriteText(std::string_view text);

    /** @brief Return whether the clipboard currently advertises native Unicode text. */
    [[nodiscard]] Result<bool> HasText();

    /** @brief Remove all current clipboard formats. */
    [[nodiscard]] VoidResult Clear();

} // namespace Sora::PAL::Clipboard
