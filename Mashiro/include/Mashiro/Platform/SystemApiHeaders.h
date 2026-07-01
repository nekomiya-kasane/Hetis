#pragma once

#ifdef PLATFORM_WINDOWS

#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <dbt.h>
#    include <dwmapi.h>
#    include <imm.h>
#    include <ole2.h>
#    include <shellapi.h>
#    include <wtsapi32.h>

#    ifndef WM_UNICHAR
#        define WM_UNICHAR 0x0109
#    endif

#    ifndef UNICODE_NOCHAR
#        define UNICODE_NOCHAR 0xFFFF
#    endif

#    ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#        define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#    endif

#    ifndef WM_WTSSESSION_CHANGE
#        define WM_WTSSESSION_CHANGE 0x02B1
#    endif

#endif
