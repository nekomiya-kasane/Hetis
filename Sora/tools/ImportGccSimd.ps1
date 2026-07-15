param(
    [Parameter(Mandatory = $true)]
    [string] $GccRoot,

    [string] $ExpectedCommit = '170fcbba4c97ea40750cc5c2fd0eb787311085f0',

    [switch] $SkipNaming,

    [switch] $SkipFormat
)

$ErrorActionPreference = 'Stop'

$actualCommit = (& git -C $GccRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $ExpectedCommit) {
    throw "GCC source commit mismatch: expected $ExpectedCommit, found $actualCommit"
}

$sourceRoot = Join-Path $GccRoot 'libstdc++-v3/include'
$targetRoot = Join-Path $PSScriptRoot '../include/Sora/Math/Simd'
$targetRoot = [System.IO.Path]::GetFullPath($targetRoot)

$files = [ordered]@{
    'std/simd'                    = 'Simd.h'
    'bits/simd_details.h'         = 'Details.h'
    'bits/vec_ops.h'              = 'VectorOperations.h'
    'bits/simd_x86.h'             = 'X86.h'
    'bits/simd_iterator.h'        = 'Iterator.h'
    'bits/simd_mask.h'            = 'Mask.h'
    'bits/simd_flags.h'           = 'Flags.h'
    'bits/simd_vec.h'             = 'Vector.h'
    'bits/simd_loadstore.h'       = 'LoadStore.h'
    'bits/simd_mask_reductions.h' = 'MaskReductions.h'
    'bits/simd_reductions.h'      = 'Reductions.h'
    'bits/simd_alg.h'             = 'Algorithms.h'
    'bits/simd_bit.h'             = 'Bit.h'
    'bits/simd_complex.h'         = 'Complex.h'
    'bits/simd_math.h'            = 'Math.h'
}

$includeNames = [ordered]@{
    'simd_details.h'         = 'Details.h'
    'vec_ops.h'              = 'VectorOperations.h'
    'simd_x86.h'             = 'X86.h'
    'simd_iterator.h'        = 'Iterator.h'
    'simd_mask.h'            = 'Mask.h'
    'simd_flags.h'           = 'Flags.h'
    'simd_vec.h'             = 'Vector.h'
    'simd_loadstore.h'       = 'LoadStore.h'
    'simd_mask_reductions.h' = 'MaskReductions.h'
    'simd_reductions.h'      = 'Reductions.h'
    'simd_alg.h'             = 'Algorithms.h'
    'simd_bit.h'             = 'Bit.h'
    'simd_complex.h'         = 'Complex.h'
    'simd_math.h'            = 'Math.h'
}

$fileBriefs = [ordered]@{
    'Details.h'          = 'Core concepts, ABI tags, traits, and utilities for SIMD types.'
    'VectorOperations.h' = 'Low-level compiler-vector operations used by the SIMD implementation.'
    'X86.h'              = 'x86 and x86-64 target-specific SIMD operations.'
    'Iterator.h'         = 'Random-access iterator for SIMD vectors and masks.'
    'Mask.h'             = 'SIMD mask types, conversions, and structural operations.'
    'Flags.h'            = 'Flags controlling SIMD conversion, alignment, load, and store operations.'
    'Vector.h'           = 'SIMD vector types and arithmetic operations.'
    'LoadStore.h'        = 'Range and iterator based SIMD load and store operations.'
    'MaskReductions.h'   = 'Boolean and index reductions over SIMD masks.'
    'Reductions.h'       = 'Arithmetic and ordered reductions over SIMD vectors.'
    'Algorithms.h'       = 'Element-wise SIMD algorithms.'
    'Bit.h'              = 'Element-wise SIMD bit operations.'
    'Complex.h'          = 'SIMD support for complex-valued vectors and masks.'
    'Math.h'             = 'Mathematical operations over SIMD vectors.'
}

New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null

foreach ($entry in $files.GetEnumerator()) {
    if ($entry.Value -eq 'Simd.h') {
        continue
    }

    $source = Join-Path $sourceRoot $entry.Key
    $target = Join-Path $targetRoot $entry.Value
    Copy-Item -LiteralPath $source -Destination $target -Force
}

Copy-Item -LiteralPath (Join-Path $GccRoot 'COPYING3') `
    -Destination (Join-Path $targetRoot 'COPYING3') -Force
Copy-Item -LiteralPath (Join-Path $GccRoot 'COPYING.RUNTIME') `
    -Destination (Join-Path $targetRoot 'COPYING.RUNTIME') -Force

$utf8 = [System.Text.UTF8Encoding]::new($false)
$promotionPattern = '(?ms)} // namespace simd\r?\n(?<imports>(?:\r?\n?  using simd::[^\r\n]+\r?\n)+)' +
                    '\r?\n?_GLIBCXX_END_NAMESPACE_VERSION\r?\n} // namespace std'

foreach ($targetName in $files.Values) {
    if ($targetName -eq 'Simd.h') {
        continue
    }

    $path = Join-Path $targetRoot $targetName
    $text = [System.IO.File]::ReadAllText($path)
    $text = [regex]::Replace($text, '(?m)^#ifndef _GLIBCXX_[A-Z0-9_]+\r?\n#define _GLIBCXX_[A-Z0-9_]+ 1\r?\n',
                             "#pragma once`n")
    $text = [regex]::Replace($text, '(?ms)^#ifdef _GLIBCXX_SYSHDR\r?\n#pragma GCC system_header\r?\n#endif\r?\n', '')
    $text = [regex]::Replace($text, '(?m)^#if __cplusplus >= 202400L\r?\n', '')
    $text = [regex]::Replace($text, '(?m)^#endif // C\+\+26\r?\n', '')
    $text = [regex]::Replace($text, '(?m)^#endif // _GLIBCXX_[A-Z0-9_]+\r?\n?', '')

    foreach ($include in $includeNames.GetEnumerator()) {
        $text = $text.Replace('"' + $include.Key + '"', '"' + $include.Value + '"')
    }
    $text = $text.Replace('#include <bits/c++config.h> // _GLIBCXX_FLOAT_IS_IEEE_BINARY32', '')
    $text = $text.Replace('#include <bits/stl_function.h> // plus, minus, multiplies, ...',
                          '#include <functional> // plus, minus, multiplies, ...')
    $text = $text.Replace('#include <bits/utility.h> // integer_sequence, etc.',
                          '#include <utility> // integer_sequence, etc.')
    $text = $text.Replace('#include <bits/utility.h>', '#include <utility>')
    $text = $text.Replace('#include <bits/stl_function.h>', '#include <functional>')
    $text = $text.Replace('#include <bits/align.h> // assume_aligned', '#include <memory> // assume_aligned')
    $text = $text.Replace('_GLIBCXX_FLOAT_IS_IEEE_BINARY32', 'SORA_SIMD_FLOAT_IS_IEEE_BINARY32')
    $text = $text.Replace('_GLIBCXX_DOUBLE_IS_IEEE_BINARY64', 'SORA_SIMD_DOUBLE_IS_IEEE_BINARY64')
    $text = $text.Replace('_GLIBCXX_CLANG', 'SORA_SIMD_CLANG')
    $text = $text.Replace('_GLIBCXX_X86', 'SORA_SIMD_X86')
    $text = $text.Replace('_GLIBCXX_SIMD_', 'SORA_SIMD_')
    $text = $text.Replace('_GLIBCXX_DELETE_SIMD', 'SORA_SIMD_DELETE')
    $text = $text.Replace('__glibcxx_simd_precondition', 'SORA_SIMD_PRECONDITION')
    $text = $text.Replace('__glibcxx_assert', 'SORA_SIMD_ASSERT')
    $text = $text.Replace('__glibcxx_on_bad_value_preserving_cast',
                          'SORA_SIMD_ON_BAD_VALUE_PRESERVING_CAST')
    $text = $text.Replace('std::__conditional_t', 'std::conditional_t')
    $text = $text.Replace('__gnu_cxx::__bfloat16_t', 'std::bfloat16_t')
    $text = [regex]::Replace(
        $text,
        'using __gnu_cxx::__int_traits;\s*constexpr auto _Nd = __int_traits<decltype\(__bits\)>::__digits;',
        'constexpr auto _Nd = std::numeric_limits<decltype(__bits)>::digits;'
    )
    $text = [regex]::Replace(
        $text,
        '(?ms)  /\*\* @internal\r?\n   \* Type used whenever no valid integer/value type exists\..*?' +
            '(?=  /\*\* @internal\r?\n   \* Alias for an unsigned integer type)',
        ''
    )
    $text = $text.Replace('_InvalidInteger', 'Detail::InvalidInteger')
    $text = $text.Replace('__integer_from', 'Detail::IntegerForSize')
    $text = $text.Replace('__float_from', 'Detail::FloatForSize')
    $text = $text.Replace('ranges::__static_sized_range', 'Detail::StaticSizedRange')
    $text = $text.Replace('ranges::__static_size', 'Detail::StaticSize')
    $text = $text.Replace('__constexpr_wrapper_like', 'Detail::ConstexprWrapperLike')
    $text = [regex]::Replace($text, '\b__unsigned_integer\b', 'Detail::UnsignedInteger')
    $text = $text.Replace('typename __make_unsigned<_From>::__type', 'Detail::MakeUnsignedT<_From>')
    $text = [regex]::Replace($text, '\b__bit_ceil\b', 'std::bit_ceil')
    $text = [regex]::Replace($text, '\b__bit_floor\b', 'std::bit_floor')
    $text = [regex]::Replace($text, '\b__bit_width\b', 'std::bit_width')
    $text = [regex]::Replace($text, '\b__has_single_bit\b', 'std::has_single_bit')
    $text = [regex]::Replace($text, '\b__countl_zero\b', 'std::countl_zero')
    $text = [regex]::Replace($text, '\b__countr_zero\b', 'std::countr_zero')
    $text = [regex]::Replace($text, '\b__in\b', 'input')
    $text = [regex]::Replace($text, '\b__to_keep\b', 'toKeep')
    $text = $text.Replace('std::is_sufficiently_aligned', 'Detail::IsSufficientlyAligned')
    $text = $text.Replace('_IotaArray', 'Detail::kIotaArray')
    $text = [regex]::Replace($text, '\bcw<', 'Detail::kConstant<')
    $text = $text.Replace('__conditional_t', 'std::conditional_t')
    $text = $text.Replace('__is_one_of', 'Detail::IsOneOf')
    $text = $text.Replace('__vec_load_return_t', 'VecLoadReturnT')
    $text = $text.Replace('__vec_load_return', 'VecLoadReturn')
    $text = $text.Replace('_S_is_const_known_equal_to', 'AreElementsConstKnownEqualTo')
    $text = $text.Replace('_M_chunk', 'ChunkStorage')
    $text = $text.Replace('__rest.size()', '__rest.kSize()')
    $text = $text.Replace('_M_get_low().size()', '_M_get_low().kSize()')
    $text = $text.Replace('(__result > __x)', '(basic_vec(__result) > __x)')
    $text = $text.Replace('(__result < __x)', '(basic_vec(__result) < __x)')
    $text = $text.Replace('__builtin_ia32_pslldqi128', 'SORA_SIMD_X86_PSLLDQI128')
    $text = $text.Replace('__builtin_ia32_psrldqi128', 'SORA_SIMD_X86_PSRLDQI128')
    foreach ($kind in @('b', 'w', 'd', 'q', 'ps', 'pd')) {
        foreach ($width in @(128, 256, 512)) {
            $blend = "__builtin_ia32_blendm${kind}_${width}_mask"
            $select = "__builtin_ia32_select${kind}_${width}"
            $text = [regex]::Replace(
                $text,
                "$([regex]::Escape($blend))\s*\(\s*__f,\s*__t,\s*__k\s*\)",
                "SORA_SIMD_X86_BLEND(__f, __t, __k, $select, $blend)"
            )
        }
    }
    $text = $text.Replace('typename __canonical_vec_type<_Tp>::type',
                          'typename __canonical_vec_type<_Tp>::Type')
    $text = $text.Replace('class __bad_value_preserving_cast', 'class BadValuePreservingCast')
    $text = $text.Replace('throw __bad_value_preserving_cast', 'throw BadValuePreservingCast')
    $text = $text.Replace('void __bad_value_preserving_cast()', 'void HandleBadValuePreservingCast()')
    $text = $text.Replace('__bad_value_preserving_cast', 'HandleBadValuePreservingCast')
    $reservedNames = [ordered]@{
        '__native_abi_t_' = 'NativeAbiT'
        '__streq_to_1'    = 'StreqTo1'
        '__is_full'       = 'fullIndices'
        '__isnan'         = 'isNanValue'
        '__init'          = 'initialValue'
        '__max'           = 'maxValue'
        '__skip'          = 'skippedIndices'
        '__0123'          = 'sequentialIndices'
        '__is0'           = 'initialIndices'
        '__iN'            = 'lastImag'
        '__rN'            = 'lastReal'
        '__rs'            = 'realChunks'
        '__all'           = 'aLongLong'
        '__bll'           = 'bLongLong'
        '__ai'            = 'aInt'
        '__bi'            = 'bInt'
        '__is'            = 'indices'
        '__id'            = 'identityValue'
    }
    foreach ($reservedName in $reservedNames.GetEnumerator()) {
        $text = [regex]::Replace(
            $text,
            "\b$([regex]::Escape($reservedName.Key))\b",
            $reservedName.Value
        )
    }
    if ($targetName -eq 'Details.h') {
        $dataTypeStart = $text.IndexOf('using _DataType')
        if ($dataTypeStart -ge 0) {
            $assertStart = $text.IndexOf('static_assert(_S_nreg == 1);', $dataTypeStart)
            if ($assertStart -ge 0) {
                $text = $text.Remove($assertStart, 'static_assert(_S_nreg == 1);'.Length)
                $text = $text.Insert(
                    $assertStart,
                    'static_assert(_S_nreg == 1 || !std::same_as<_Tp, _Tp>);'
                )
            }
        }
        $maskDataTypeStart = $text.IndexOf('using _MaskDataType')
        if ($maskDataTypeStart -ge 0) {
            $assertStart = $text.IndexOf('static_assert(_S_nreg == 1);', $maskDataTypeStart)
            if ($assertStart -ge 0) {
                $text = $text.Remove($assertStart, 'static_assert(_S_nreg == 1);'.Length)
                $text = $text.Insert($assertStart, 'static_assert(_S_nreg == 1 || _Bytes != _Bytes);')
            }
        }
    }
    $text = $text.Replace('constexpr auto [...', 'const auto& [...')
    $text = [regex]::Replace($text, '(?m)^\s*using _Base::operator basic_vec;\r?\n', '')
    $text = $text.Replace('const _Cx __cx(_Tc(__x[__i]), _Tc(__x[__i + 1]));',
                          'const _Cx __cx{_Tc(__x[__i]), _Tc(__x[__i + 1])};')
    $text = $text.Replace('const _Cx __cy(_Tc(__y[__i]), _Tc(__y[__i + 1]));',
                          'const _Cx __cy{_Tc(__y[__i]), _Tc(__y[__i + 1])};')
    if ($targetName -eq 'X86.h') {
        $x86TypeMapping = @'
  template <typename _Tp>
    [[nodiscard]] consteval auto
    __x86_intrin_int_type()
    {
      if constexpr (sizeof(_Tp) == 1)
        return std::type_identity<char>{};
      else
        return std::type_identity<Detail::IntegerForSize<sizeof(_Tp)>>{};
    }

  template <typename _Tp>
    using __x86_intrin_int = typename decltype(__x86_intrin_int_type<_Tp>())::type;

  template <typename _Tp>
    [[nodiscard]] consteval auto
    __x86_intrin_type_impl()
    {
      if constexpr (is_integral_v<_Tp> || sizeof(_Tp) <= 2)
        return std::type_identity<__x86_intrin_int<_Tp>>{};
      else
        return std::type_identity<__canonical_vec_type_t<_Tp>>{};
    }

  template <typename _Tp>
    using __x86_intrin_type = typename decltype(__x86_intrin_type_impl<_Tp>())::type;

  template <typename _Tp>
    [[nodiscard]] consteval auto
    __x86_intel_intrin_value_type_impl()
    {
      if constexpr (is_integral_v<_Tp>)
        return std::type_identity<long long>{};
      else if constexpr (sizeof(_Tp) == 8)
        return std::type_identity<double>{};
      else if constexpr (sizeof(_Tp) == 4)
        return std::type_identity<float>{};
      else if constexpr (sizeof(_Tp) == 2)
        return std::type_identity<_Float16>{};
    }

  template <typename _Tp>
    using __x86_intel_intrin_value_type
      = typename decltype(__x86_intel_intrin_value_type_impl<_Tp>())::type;

'@
        $text = [regex]::Replace(
            $text,
            '(?ms)  template <typename _Tp>\r?\n    using __x86_intrin_int.*?(?=#if !SORA_SIMD_CLANG)',
            $x86TypeMapping
        )
    }
    $standardNames = @(
        'array',
        'assume_aligned',
        'bit_and',
        'bit_cast',
        'bit_or',
        'bit_xor',
        'bitset',
        'bool_constant',
        'byte',
        'common_type_t',
        'complex',
        'conditional_t',
        'contiguous_iterator',
        'convertible_to',
        'declval',
        'default_sentinel_t',
        'destructible',
        'dynamic_extent',
        'floating_point',
        'greater',
        'indirectly_writable',
        'input_iterator_tag',
        'initializer_list',
        'int16_t',
        'int32_t',
        'int64_t',
        'int8_t',
        'integral',
        'integral_constant',
        'integer_sequence',
        'iter_difference_t',
        'iter_value_t',
        'is_arithmetic_v',
        'is_base_of_v',
        'is_class_v',
        'is_const_v',
        'is_default_constructible_v',
        'is_enum_v',
        'is_floating_point_v',
        'is_integral_v',
        'is_pointer_v',
        'is_same_v',
        'is_signed_v',
        'is_sufficiently_aligned',
        'is_unsigned_v',
        'less',
        'make_integer_sequence',
        'make_signed_t',
        'make_unsigned_t',
        'minus',
        'move',
        'multiplies',
        'negate',
        'numeric_limits',
        'pair',
        'plus',
        'ptrdiff_t',
        'random_access_iterator_tag',
        'ranges',
        'remove_const_t',
        'remove_cv_t',
        'remove_cvref_t',
        'same_as',
        'signed_integral',
        'size_t',
        'sized_sentinel_for',
        'span',
        'totally_ordered',
        'tuple',
        'tuple_size_v',
        'type_identity_t',
        'uint16_t',
        'uint32_t',
        'uint64_t',
        'uint8_t',
        'uint_least16_t',
        'uint_least32_t',
        'underlying_type_t',
        'unsigned_integral'
    )
    foreach ($standardName in $standardNames) {
        $text = [regex]::Replace($text, "(?<![A-Za-z0-9_:])$standardName\b", "std::$standardName")
    }
    $text = $text.Replace('std::is_sufficiently_aligned', 'Detail::IsSufficientlyAligned')
    $text = $text.Replace('#include <std::', '#include <')

    $publicNames = [ordered]@{
        'basic_vec'       = 'BasicVector'
        'basic_mask'      = 'BasicMask'
        'alignment_v'     = 'kAlignment'
        'flag_overaligned'= 'kOveralignedFlag'
        'flag_default'    = 'kDefaultFlag'
        'flag_convert'    = 'kConvertFlag'
        'flag_aligned'    = 'kAlignedFlag'
        'rebind_t'        = 'Rebind'
        'resize_t'        = 'Resize'
        'const_iterator'  = 'ConstIteratorType'
        'iterator'        = 'IteratorType'
        'mask_type'       = 'MaskType'
        'abi_type'        = 'AbiType'
        'rebind'          = 'RebindTraits'
        'resize'          = 'ResizeTraits'
        'alignment'       = 'Alignment'
        'flags'           = 'Flags'
        'mask'            = 'Mask'
        'vec'             = 'Vector'
    }
    foreach ($publicName in $publicNames.GetEnumerator()) {
        $text = [regex]::Replace($text, "\b$($publicName.Key)\b", $publicName.Value)
    }
    $text = $text.Replace('/// C++26 [simd.Mask.ctor] uses unconditional explicit',
                          '// C++26 [simd.mask.ctor] uses unconditional explicit')

    $text = [regex]::Replace($text, '\b_S_size\b', 'storageSize')
    $text = [regex]::Replace($text, '\b_M_real\b', 'realData')
    $text = [regex]::Replace($text, '\b_M_imag\b', 'imagData')
    if ($targetName -eq 'Mask.h') {
        $text = [regex]::Replace(
            $text,
            '(?s)int __offset = -_Offset\.value;.*?return _Dst\(__r\);',
            { param($match) $match.Value.Replace('__offset', 'runningOffset') }
        )
        $text = [regex]::Replace(
            $text,
            '(?s)int __offset = 0;.*?__builtin_memcpy\(&__r, __tmp \+ _Offset\.value, sizeof\(_Ret\)\);',
            { param($match) $match.Value.Replace('__offset', 'runningOffset') }
        )
    }
    $text = [regex]::Replace($text, '\b_Flags\b', 'FlagTypes')
    $text = [regex]::Replace($text, '\b_M_([A-Za-z0-9_]+)\b', '$1')
    $text = [regex]::Replace($text, '\b_S_([A-Za-z0-9_]+)\b', '$1')
    $text = [regex]::Replace($text, '\b_(?!Float(?:16|32|64|128)\b)([A-Z][A-Za-z0-9]*)\b', '$1')

    $simdTypeNames = '(?:Vp|Mp|RV|TV|V0|V1|IV|Ip|Vs|VecType|Dst0|Dst1)'
    $text = [regex]::Replace($text, "\b($simdTypeNames)::size\b", '$1::kSize')
    $text = [regex]::Replace($text, "\btypename ($simdTypeNames)::value_type\b", 'typename $1::ValueType')
    $text = [regex]::Replace(
        $text,
        '(?m)(RebindTraits|ResizeTraits|VecLoadReturn)([^\r\n]*)::type\b',
        '$1$2::Type'
    )
    $text = [regex]::Replace(
        $text,
        '(?m)(CanonicalVecType<[^\r\n]*>)::type\b',
        '$1::Type'
    )
    $text = [regex]::Replace(
        $text,
        '(Basic(?:Vector|Mask)<[^\r\n]*>)::size\b',
        '$1::kSize'
    )

    $text = [regex]::Replace(
        $text,
        'namespace std _GLIBCXX_VISIBILITY\(default\)\r?\n\{\r?\n_GLIBCXX_BEGIN_NAMESPACE_VERSION' +
            '\r?\nnamespace simd\r?\n\{',
        'namespace Sora::Math::Simd {'
    )
    $text = [regex]::Replace(
        $text,
        'namespace std _GLIBCXX_VISIBILITY\(default\)\r?\n\{\r?\n_GLIBCXX_BEGIN_NAMESPACE_VERSION' +
            '\r?\n\r?\n  template<typename> class (?:std::)?complex;\r?\n\r?\nnamespace simd\r?\n\{',
        'namespace Sora::Math::Simd {'
    )
    $text = [regex]::Replace($text, '(?m)^namespace simd\r?\n\{$', 'namespace Sora::Math::Simd {')
    $text = [regex]::Replace($text, $promotionPattern, {
        param($match)
        $imports = $match.Groups['imports'].Value.Replace('using simd::', 'using Simd::')
        return "} // namespace Sora::Math::Simd`n`nnamespace Sora::Math {$imports`n} // namespace Sora::Math"
    })
    $text = [regex]::Replace(
        $text,
        '} // namespace simd\r?\n_GLIBCXX_END_NAMESPACE_VERSION\r?\n} // namespace std',
        '} // namespace Sora::Math::Simd'
    )
    $text = $text.Replace('} // namespace simd', '} // namespace Sora::Math::Simd')
    $text = $text.Replace('std::simd::', 'Sora::Math::Simd::')
    $text = $text.Replace('simd::', 'Sora::Math::Simd::')
    if ($targetName -eq 'Details.h') {
        $text = $text.Replace('#pragma once', "#pragma once`n`n#include `"Portability.h`"")
        $text = [regex]::Replace(
            $text,
            '(?ms)#if defined __x86_64__ \|\| defined __i386__\r?\n#define SORA_SIMD_X86 1\r?\n' +
                '#else\r?\n#define SORA_SIMD_X86 0\r?\n#endif\r?\n',
            ''
        )
    }

    if ($text -notmatch '@file\s') {
        $fileHeader = @"
/**
 * @file $targetName
 * @brief $($fileBriefs[$targetName])
 * @ingroup Math
 */

"@
        $pragmaIndex = $text.IndexOf('#pragma once')
        if ($pragmaIndex -lt 0) {
            throw "Could not locate #pragma once in $targetName"
        }
        $text = $text.Insert($pragmaIndex, $fileHeader)
    }

    $text = $text -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($path, $text, $utf8)
}

$simdHeader = @'
// Copyright The GNU Toolchain Authors.
//
// This file is part of the GNU ISO C++ Library. This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 3,
// or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// Under Section 7 of GPL version 3, you are granted additional permissions described in the GCC Runtime Library
// Exception, version 3.1, as published by the Free Software Foundation.
//
// You should have received a copy of the GNU General Public License and a copy of the GCC Runtime Library Exception
// along with this program; see the files COPYING3 and COPYING.RUNTIME respectively. If not, see
// <http://www.gnu.org/licenses/>.

/**
 * @file Simd.h
 * @brief C++26 data-parallel vector and mask types.
 * @ingroup Math
 */

#pragma once

#include "Vector.h"
#include "LoadStore.h"
#include "MaskReductions.h"
#include "Reductions.h"
#include "Algorithms.h"
#include "Bit.h"
#include "Complex.h"
#include "Math.h"
'@
[System.IO.File]::WriteAllText((Join-Path $targetRoot 'Simd.h'), $simdHeader + "`n", $utf8)

if (-not $SkipNaming) {
    & (Join-Path $PSScriptRoot 'ApplySimdNaming.ps1') `
        -IncludeRoot (Join-Path $PSScriptRoot '../include') `
        -TargetRoot $targetRoot
}

if (-not $SkipFormat) {
    foreach ($header in Get-ChildItem -LiteralPath $targetRoot -Filter '*.h') {
        & clang-format -i -style=file $header.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "clang-format failed for $($header.FullName) with exit code $LASTEXITCODE"
        }
    }
}
