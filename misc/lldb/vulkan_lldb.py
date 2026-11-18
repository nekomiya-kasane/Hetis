"""Repository LLDB visualizers for the COCA clang-p2996 toolchain.

The Windows COCA profile uses the MSVC ABI and normally exposes MSVC STL layouts to LLDB. The module also imports the
bundled libc++ formatter so explicit libc++ builds continue to get the official container and string visualizers.
"""

import importlib
import pathlib
import lldb


_MAX_RECURSION = 8


def _raw(value):
    try:
        raw = value.GetNonSyntheticValue()
        if raw and raw.IsValid():
            return raw
    except Exception:
        pass
    return value


def _resolved(value):
    value = _raw(value)
    try:
        if value.GetType().IsReferenceType():
            dereferenced = value.Dereference()
            if dereferenced and dereferenced.IsValid():
                return _raw(dereferenced)
    except Exception:
        pass
    return value


def _child(value, *names):
    current = _resolved(value)
    for name in names:
        current = current.GetChildMemberWithName(name)
        if not current or not current.IsValid():
            return None
    return current


def _find(value, name, depth=_MAX_RECURSION):
    value = _resolved(value)
    direct = value.GetChildMemberWithName(name)
    if direct and direct.IsValid():
        return direct
    if depth <= 0:
        return None
    for index in range(value.GetNumChildren()):
        found = _find(value.GetChildAtIndex(index), name, depth - 1)
        if found is not None:
            return found
    return None


def _unsigned(value, default=0):
    if value is None or not value.IsValid():
        return default
    return value.GetValueAsUnsigned(default)


def _truth(value):
    if value is None or not value.IsValid():
        return None
    text = value.GetValue()
    if text is not None:
        lowered = text.lower()
        if lowered in ("true", "1"):
            return True
        if lowered in ("false", "0"):
            return False
    return bool(value.GetValueAsUnsigned(0))


def _bool_child(value, *names):
    return _truth(_child(value, *names))


def _summary(value):
    if value is None or not value.IsValid():
        return "<unavailable>"
    return value.GetSummary() or value.GetValue() or "<available>"


def _msvc_vector_storage(value):
    return _child(value, "_Mypair", "_Myval2")


def _msvc_vector_bounds(value):
    storage = _msvc_vector_storage(value)
    if storage is None:
        return None
    first = storage.GetChildMemberWithName("_Myfirst")
    last = storage.GetChildMemberWithName("_Mylast")
    end = storage.GetChildMemberWithName("_Myend")
    if not first.IsValid() or not last.IsValid() or not end.IsValid():
        return None
    element_type = first.GetType().GetPointeeType()
    element_size = element_type.GetByteSize()
    if element_size == 0:
        return None
    first_addr = first.GetValueAsUnsigned(0)
    last_addr = last.GetValueAsUnsigned(0)
    end_addr = end.GetValueAsUnsigned(0)
    size = max(0, (last_addr - first_addr) // element_size)
    capacity = max(0, (end_addr - first_addr) // element_size)
    return first_addr, size, capacity, element_type, element_size


def _msvc_vector_summary(value, _internal_dict):
    bounds = _msvc_vector_bounds(value)
    if bounds is None:
        return "size=<unavailable>"
    _, size, capacity, _, _ = bounds
    return f"size={size}, capacity={capacity}"


class MsvcVectorProvider:
    """Synthetic children for MSVC STL std::vector."""

    def __init__(self, value, _internal_dict):
        self.value = _resolved(value)
        self.first = 0
        self.size = 0
        self.element_type = None
        self.element_size = 0
        self.update()

    def update(self):
        bounds = _msvc_vector_bounds(self.value)
        if bounds is None:
            self.first = 0
            self.size = 0
            self.element_type = None
            self.element_size = 0
            return
        self.first, self.size, _, self.element_type, self.element_size = bounds

    def has_children(self):
        return self.size != 0

    def num_children(self):
        return self.size

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]"):
            try:
                index = int(name[1:-1])
                return index if 0 <= index < self.size else -1
            except ValueError:
                return -1
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.size or self.element_type is None:
            return None
        address = self.first + index * self.element_size
        return self.value.CreateValueFromAddress(f"[{index}]", address, self.element_type)


def _msvc_string_summary(value, _internal_dict):
    storage = _child(value, "_Mypair", "_Myval2")
    if storage is None:
        return "<unavailable>"
    size = _unsigned(storage.GetChildMemberWithName("_Mysize"))
    capacity = _unsigned(storage.GetChildMemberWithName("_Myres"))
    bx = storage.GetChildMemberWithName("_Bx")
    if not bx or not bx.IsValid():
        return f"size={size}"

    small_capacity = 15
    if capacity <= small_capacity:
        data = bx.GetChildMemberWithName("_Buf")
        address = _unsigned(data.AddressOf())
    else:
        data = bx.GetChildMemberWithName("_Ptr")
        address = _unsigned(data)
    error = lldb.SBError()
    raw = value.GetProcess().ReadMemory(address, size, error)
    if not error.Success():
        return f"size={size}"
    text = raw.decode("utf-8", "replace")
    return f'"{text}" size={size}'


def _optional_summary(value, _internal_dict):
    value = _resolved(value)
    engaged = _bool_child(value, "__engaged_")
    if engaged is None:
        engaged_member = _find(value, "_Has_value")
        engaged = _truth(engaged_member) if engaged_member is not None else None
    if engaged is None:
        engaged = _bool_child(value, "__base_", "__engaged_")
    if not engaged:
        return "has_value=false"
    stored = _find(value, "_Value") or _find(value, "__val_")
    return f"has_value=true, value={_summary(stored)}"


def _expected_summary(value, _internal_dict):
    value = _resolved(value)
    has_value_member = _find(value, "_Has_value")
    if has_value_member is not None:
        if _truth(has_value_member):
            return f"has_value=true, value={_summary(_find(value, '_Value'))}"
        return f"has_value=false, error={_summary(_find(value, '_Unexpected'))}"

    has_value = _bool_child(value, "__repr_", "__v", "__has_val_")
    if has_value is None:
        return "state=<unavailable>"
    return "has_value=true" if has_value else "has_value=false"


def __lldb_init_module(debugger, _internal_dict):
    libcxx = importlib.import_module("lldb.formatters.cpp.libcxx")
    libcxx_path = pathlib.Path(libcxx.__file__).as_posix()
    debugger.HandleCommand(f'command script import "{libcxx_path}"')
    debugger.HandleCommand("type category enable libcxx")

    debugger.HandleCommand("type category define vulkan-msvc-stl")
    debugger.HandleCommand(
        "type synthetic add -l vulkan_lldb.MsvcVectorProvider "
        "-x '^std::vector<.+>$' -w vulkan-msvc-stl"
    )
    debugger.HandleCommand(
        "type summary add -F vulkan_lldb._msvc_vector_summary "
        "-e -x '^std::vector<.+>$' -w vulkan-msvc-stl"
    )
    debugger.HandleCommand(
        "type summary add -F vulkan_lldb._msvc_string_summary "
        "-x '^std::basic_string<char,.+>$' -w vulkan-msvc-stl"
    )
    debugger.HandleCommand(
        "type summary add -F vulkan_lldb._optional_summary "
        "-x '^std::(__[[:alnum:]_]+::)?optional<.+>$' -w vulkan-msvc-stl"
    )
    debugger.HandleCommand(
        "type summary add -F vulkan_lldb._expected_summary "
        "-x '^std::(__[[:alnum:]_]+::)?expected<.+>$' -w vulkan-msvc-stl"
    )
    debugger.HandleCommand("type category enable vulkan-msvc-stl")
