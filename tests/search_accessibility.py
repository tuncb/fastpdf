"""Read native result row names and bounds through MSAA, without desktop input."""
import ctypes as c
from ctypes import wintypes as w
import uuid


class Variant(c.Structure):
    _fields_ = [("vt", c.c_ushort), ("reserved", c.c_ushort * 3), ("data", c.c_longlong * 2)]


def result_row(control, index):
    ole = c.OleDLL("ole32")
    ole.CoInitialize(None)
    ole.CoUninitialize.restype = None
    auto = c.OleDLL("oleaut32")
    auto.SysFreeString.argtypes = [c.c_void_p]
    auto.SysFreeString.restype = None
    call = c.OleDLL("oleacc").AccessibleObjectFromWindow
    call.argtypes = [w.HWND, w.DWORD, c.c_void_p, c.POINTER(c.c_void_p)]
    iid = (c.c_ubyte * 16).from_buffer_copy(uuid.UUID("618736e0-3c3d-11cf-810c-00aa00389b71").bytes_le)
    value = c.c_void_p()
    call(control, 0xFFFFFFFC, c.byref(iid), c.byref(value))  # OBJID_CLIENT, IAccessible
    table = c.cast(value, c.POINTER(c.POINTER(c.c_void_p))).contents
    child = Variant()
    child.vt, child.data[0] = 3, index + 1  # VT_I4, one-based accessible child ID
    string = c.c_void_p()
    try:
        hr = c.WINFUNCTYPE(c.c_long, c.c_void_p, Variant, c.POINTER(c.c_void_p))(table[10])(
            value, child, c.byref(string))
        assert hr == 0, f"Cannot read accessible row {index}: {hr}"
        name = c.wstring_at(string) if string.value else ""
        bounds = [c.c_long() for _ in range(4)]
        hr = c.WINFUNCTYPE(c.c_long, c.c_void_p, *([c.POINTER(c.c_long)] * 4), Variant)(table[22])(
            value, *(c.byref(part) for part in bounds), child)
        assert hr == 0, f"Cannot read row bounds: {hr}"
        return name, tuple(part.value for part in bounds)
    finally:
        if string.value:
            auto.SysFreeString(string)
        c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(value)
        ole.CoUninitialize()
