#!/usr/bin/env python3
"""Capture and plot SpscDemo scheduling with the native Windows ETW APIs."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import uuid


ERROR_SUCCESS = 0
ERROR_ACCESS_DENIED = 5
ERROR_ALREADY_EXISTS = 183
EVENT_TRACE_CONTROL_STOP = 1
EVENT_TRACE_FILE_MODE_SEQUENTIAL = 0x00000001
EVENT_TRACE_SYSTEM_LOGGER_MODE = 0x02000000
EVENT_TRACE_FLAG_PROCESS = 0x00000001
EVENT_TRACE_FLAG_THREAD = 0x00000002
EVENT_TRACE_FLAG_CSWITCH = 0x00000010
EVENT_TRACE_FLAG_DISPATCHER = 0x00000800
PROCESS_TRACE_MODE_RAW_TIMESTAMP = 0x00001000
PROCESS_TRACE_MODE_EVENT_RECORD = 0x10000000
WNODE_FLAG_TRACED_GUID = 0x00020000
EVENT_HEADER_FLAG_32_BIT_HEADER = 0x0020
INVALID_PROCESSTRACE_HANDLE = (1 << 64) - 1

THREAD_STATES = {
    0: "Initialized", 1: "Ready", 2: "Running", 3: "Standby", 4: "Terminated",
    5: "Waiting", 6: "Transition", 7: "DeferredReady", 8: "GateWaitObsolete",
    9: "WaitingForProcessInSwap",
}

WAIT_REASONS = {
    0: "Executive", 1: "FreePage", 2: "PageIn", 3: "PoolAllocation", 4: "DelayExecution",
    5: "Suspended", 6: "UserRequest", 7: "WrExecutive", 8: "WrFreePage", 9: "WrPageIn",
    10: "WrPoolAllocation", 11: "WrDelayExecution", 12: "WrSuspended", 13: "WrUserRequest",
    14: "WrEventPair", 15: "WrQueue", 16: "WrLpcReceive", 17: "WrLpcReply",
    18: "WrVirtualMemory", 19: "WrPageOut", 20: "WrRendezvous", 21: "WrKeyedEvent",
    22: "WrTerminated", 23: "WrProcessInSwap", 24: "WrCpuRateControl", 25: "WrCalloutStack",
    26: "WrKernel", 27: "WrResource", 28: "WrPushLock", 29: "WrMutex", 30: "WrQuantumEnd",
    31: "WrDispatchInt", 32: "WrPreempted", 33: "WrYieldExecution", 34: "WrFastMutex",
    35: "WrGuardedMutex", 36: "WrRundown", 37: "WrAlertByThreadId", 38: "WrDeferredPreempt",
    39: "WrPhysicalFault", 40: "WrIoRing", 41: "WrMdlCache", 42: "WrRcu",
}

class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]

    @classmethod
    def from_uuid(cls, value: uuid.UUID) -> "GUID":
        return cls(value.time_low, value.time_mid, value.time_hi_version, (ctypes.c_ubyte * 8)(*value.bytes[8:]))


class WNODE_HEADER(ctypes.Structure):
    _fields_ = [
        ("BufferSize", wintypes.ULONG),
        ("ProviderId", wintypes.ULONG),
        ("HistoricalContext", ctypes.c_uint64),
        ("TimeStamp", ctypes.c_int64),
        ("Guid", GUID),
        ("ClientContext", wintypes.ULONG),
        ("Flags", wintypes.ULONG),
    ]


class EVENT_TRACE_PROPERTIES(ctypes.Structure):
    _fields_ = [
        ("Wnode", WNODE_HEADER),
        ("BufferSize", wintypes.ULONG),
        ("MinimumBuffers", wintypes.ULONG),
        ("MaximumBuffers", wintypes.ULONG),
        ("MaximumFileSize", wintypes.ULONG),
        ("LogFileMode", wintypes.ULONG),
        ("FlushTimer", wintypes.ULONG),
        ("EnableFlags", wintypes.ULONG),
        ("AgeLimit", wintypes.LONG),
        ("NumberOfBuffers", wintypes.ULONG),
        ("FreeBuffers", wintypes.ULONG),
        ("EventsLost", wintypes.ULONG),
        ("BuffersWritten", wintypes.ULONG),
        ("LogBuffersLost", wintypes.ULONG),
        ("RealTimeBuffersLost", wintypes.ULONG),
        ("LoggerThreadId", wintypes.HANDLE),
        ("LogFileNameOffset", wintypes.ULONG),
        ("LoggerNameOffset", wintypes.ULONG),
    ]


class EVENT_DESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("Id", wintypes.WORD),
        ("Version", ctypes.c_ubyte),
        ("Channel", ctypes.c_ubyte),
        ("Level", ctypes.c_ubyte),
        ("Opcode", ctypes.c_ubyte),
        ("Task", wintypes.WORD),
        ("Keyword", ctypes.c_uint64),
    ]


class EVENT_HEADER(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.WORD),
        ("HeaderType", wintypes.WORD),
        ("Flags", wintypes.WORD),
        ("EventProperty", wintypes.WORD),
        ("ThreadId", wintypes.DWORD),
        ("ProcessId", wintypes.DWORD),
        ("TimeStamp", ctypes.c_int64),
        ("ProviderId", GUID),
        ("EventDescriptor", EVENT_DESCRIPTOR),
        ("KernelTime", wintypes.ULONG),
        ("UserTime", wintypes.ULONG),
        ("ActivityId", GUID),
    ]


class ETW_BUFFER_CONTEXT(ctypes.Structure):
    _fields_ = [("ProcessorIndex", wintypes.WORD), ("LoggerId", wintypes.WORD)]


class EVENT_HEADER_EXTENDED_DATA_ITEM(ctypes.Structure):
    _fields_ = [
        ("Reserved1", wintypes.WORD),
        ("ExtType", wintypes.WORD),
        ("Linkage", wintypes.WORD),
        ("DataSize", wintypes.WORD),
        ("DataPtr", ctypes.c_uint64),
    ]


class EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("EventHeader", EVENT_HEADER),
        ("BufferContext", ETW_BUFFER_CONTEXT),
        ("ExtendedDataCount", wintypes.WORD),
        ("UserDataLength", wintypes.WORD),
        ("ExtendedData", ctypes.POINTER(EVENT_HEADER_EXTENDED_DATA_ITEM)),
        ("UserData", ctypes.c_void_p),
        ("UserContext", ctypes.c_void_p),
    ]


class EVENT_TRACE_HEADER_CLASS(ctypes.Structure):
    _fields_ = [("Type", ctypes.c_ubyte), ("Level", ctypes.c_ubyte), ("Version", wintypes.WORD)]


class EVENT_TRACE_HEADER(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.WORD),
        ("HeaderType", ctypes.c_ubyte),
        ("MarkerFlags", ctypes.c_ubyte),
        ("Class", EVENT_TRACE_HEADER_CLASS),
        ("ThreadId", wintypes.DWORD),
        ("ProcessId", wintypes.DWORD),
        ("TimeStamp", ctypes.c_int64),
        ("Guid", GUID),
        ("ClientContext", wintypes.ULONG),
        ("Flags", wintypes.ULONG),
    ]


class EVENT_TRACE(ctypes.Structure):
    _fields_ = [
        ("Header", EVENT_TRACE_HEADER),
        ("InstanceId", wintypes.ULONG),
        ("ParentInstanceId", wintypes.ULONG),
        ("ParentGuid", GUID),
        ("MofData", ctypes.c_void_p),
        ("MofLength", wintypes.ULONG),
        ("ClientContext", wintypes.ULONG),
    ]


class SYSTEMTIME(ctypes.Structure):
    _fields_ = [(name, wintypes.WORD) for name in (
        "Year", "Month", "DayOfWeek", "Day", "Hour", "Minute", "Second", "Milliseconds"
    )]


class TIME_ZONE_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("Bias", wintypes.LONG),
        ("StandardName", wintypes.WCHAR * 32),
        ("StandardDate", SYSTEMTIME),
        ("StandardBias", wintypes.LONG),
        ("DaylightName", wintypes.WCHAR * 32),
        ("DaylightDate", SYSTEMTIME),
        ("DaylightBias", wintypes.LONG),
    ]


class TRACE_LOGFILE_HEADER(ctypes.Structure):
    _fields_ = [
        ("BufferSize", wintypes.ULONG),
        ("Version", wintypes.ULONG),
        ("ProviderVersion", wintypes.ULONG),
        ("NumberOfProcessors", wintypes.ULONG),
        ("EndTime", ctypes.c_int64),
        ("TimerResolution", wintypes.ULONG),
        ("MaximumFileSize", wintypes.ULONG),
        ("LogFileMode", wintypes.ULONG),
        ("BuffersWritten", wintypes.ULONG),
        ("StartBuffers", wintypes.ULONG),
        ("PointerSize", wintypes.ULONG),
        ("EventsLost", wintypes.ULONG),
        ("CpuSpeedInMHz", wintypes.ULONG),
        ("LoggerName", wintypes.LPWSTR),
        ("LogFileName", wintypes.LPWSTR),
        ("TimeZone", TIME_ZONE_INFORMATION),
        ("BootTime", ctypes.c_int64),
        ("PerfFreq", ctypes.c_int64),
        ("StartTime", ctypes.c_int64),
        ("ReservedFlags", wintypes.ULONG),
        ("BuffersLost", wintypes.ULONG),
    ]


class EVENT_TRACE_LOGFILEW(ctypes.Structure):
    pass


EVENT_RECORD_CALLBACK = ctypes.WINFUNCTYPE(None, ctypes.POINTER(EVENT_RECORD))
BUFFER_CALLBACK = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.POINTER(EVENT_TRACE_LOGFILEW))
EVENT_TRACE_LOGFILEW._fields_ = [
    ("LogFileName", wintypes.LPWSTR),
    ("LoggerName", wintypes.LPWSTR),
    ("CurrentTime", ctypes.c_int64),
    ("BuffersRead", wintypes.ULONG),
    ("ProcessTraceMode", wintypes.ULONG),
    ("CurrentEvent", EVENT_TRACE),
    ("LogfileHeader", TRACE_LOGFILE_HEADER),
    ("BufferCallback", BUFFER_CALLBACK),
    ("BufferSize", wintypes.ULONG),
    ("Filled", wintypes.ULONG),
    ("EventsLost", wintypes.ULONG),
    ("EventRecordCallback", EVENT_RECORD_CALLBACK),
    ("IsKernelTrace", wintypes.ULONG),
    ("Context", ctypes.c_void_p),
]


class TracePropertiesBuffer(ctypes.Structure):
    _fields_ = [
        ("properties", EVENT_TRACE_PROPERTIES),
        ("logger_name", wintypes.WCHAR * 128),
        ("log_file_name", wintypes.WCHAR * 1024),
    ]


class KernelSchedulerTrace:
    """Owner of an ETW system logger that records natively and decodes the ETL after capture."""

    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("Live scheduler capture is available only on Windows")
        self._advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._configure_apis()
        self._name = f"Sora Python Scheduler Trace {os.getpid()} {time.time_ns()}"
        self._properties = TracePropertiesBuffer()
        self._producer = ctypes.c_uint64()
        self._consumer = ctypes.c_uint64(INVALID_PROCESSTRACE_HANDLE)
        self._logfile = EVENT_TRACE_LOGFILEW()
        self._etl_path: Path | None = None
        self._callback_error: BaseException | None = None
        self._buffer_events_lost = 0
        self._target_pid: int | None = None
        self._target_tids: set[int] = set()
        self.context_switches: list[tuple[int, ...]] = []
        self.ready_events: list[tuple[int, ...]] = []
        self.thread_events: list[tuple[str, int, int, int, int]] = []
        self.thread_name_events: list[tuple[int, int, int, int, str]] = []
        self.process_events: list[tuple[str, int, int, int, int, int]] = []
        self._event_callback = EVENT_RECORD_CALLBACK(self._on_event)
        self._buffer_callback = BUFFER_CALLBACK(self._on_buffer)

    def _configure_apis(self) -> None:
        self._advapi32.StartTraceW.argtypes = [ctypes.POINTER(ctypes.c_uint64), wintypes.LPCWSTR,
                                               ctypes.POINTER(EVENT_TRACE_PROPERTIES)]
        self._advapi32.StartTraceW.restype = wintypes.ULONG
        self._advapi32.ControlTraceW.argtypes = [ctypes.c_uint64, wintypes.LPCWSTR,
                                                 ctypes.POINTER(EVENT_TRACE_PROPERTIES), wintypes.ULONG]
        self._advapi32.ControlTraceW.restype = wintypes.ULONG
        self._advapi32.OpenTraceW.argtypes = [ctypes.POINTER(EVENT_TRACE_LOGFILEW)]
        self._advapi32.OpenTraceW.restype = ctypes.c_uint64
        self._advapi32.ProcessTrace.argtypes = [ctypes.POINTER(ctypes.c_uint64), wintypes.ULONG,
                                                ctypes.c_void_p, ctypes.c_void_p]
        self._advapi32.ProcessTrace.restype = wintypes.ULONG
        self._advapi32.CloseTrace.argtypes = [ctypes.c_uint64]
        self._advapi32.CloseTrace.restype = wintypes.ULONG
        self._kernel32.QueryPerformanceFrequency.argtypes = [ctypes.POINTER(ctypes.c_int64)]
        self._kernel32.QueryPerformanceFrequency.restype = wintypes.BOOL
        self._kernel32.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_int64)]
        self._kernel32.QueryPerformanceCounter.restype = wintypes.BOOL

    def qpc(self) -> int:
        value = ctypes.c_int64()
        if not self._kernel32.QueryPerformanceCounter(ctypes.byref(value)):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.value

    def set_target_pid(self, process_id: int) -> None:
        self._target_pid = process_id
        self._target_tids.update(event[4] for event in self.thread_events if event[3] == process_id)

    def start(self, etl_path: Path) -> None:
        self._etl_path = etl_path.resolve()
        properties = self._properties.properties
        properties.Wnode.BufferSize = ctypes.sizeof(self._properties)
        properties.Wnode.Guid = GUID.from_uuid(uuid.uuid4())
        properties.Wnode.ClientContext = 1
        properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID
        properties.BufferSize = 64
        properties.MinimumBuffers = 64
        properties.MaximumBuffers = 256
        properties.MaximumFileSize = 256
        properties.FlushTimer = 1
        properties.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_SYSTEM_LOGGER_MODE
        properties.EnableFlags = (EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_CSWITCH |
                                  EVENT_TRACE_FLAG_DISPATCHER)
        properties.LoggerNameOffset = TracePropertiesBuffer.logger_name.offset
        properties.LogFileNameOffset = TracePropertiesBuffer.log_file_name.offset
        self._properties.logger_name = self._name
        self._properties.log_file_name = str(self._etl_path)

        frequency = ctypes.c_int64()
        if not self._kernel32.QueryPerformanceFrequency(ctypes.byref(frequency)) or frequency.value <= 0:
            raise ctypes.WinError(ctypes.get_last_error())
        self.ticks_per_second = frequency.value

        status = self._advapi32.StartTraceW(ctypes.byref(self._producer), self._name,
                                            ctypes.byref(self._properties.properties))
        if status != ERROR_SUCCESS:
            self._raise_status("StartTraceW", status)

    def stop_collection(self) -> None:
        if not self._producer.value:
            return
        status = self._advapi32.ControlTraceW(self._producer, self._name,
                                              ctypes.byref(self._properties.properties),
                                              EVENT_TRACE_CONTROL_STOP)
        self._producer.value = 0
        if status != ERROR_SUCCESS:
            self._raise_status("ControlTraceW", status)

    def consume(self) -> None:
        if self._etl_path is None:
            raise RuntimeError("ETW trace has no ETL path")
        self._logfile = EVENT_TRACE_LOGFILEW()
        self._logfile.LogFileName = str(self._etl_path)
        self._logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP
        self._logfile.EventRecordCallback = self._event_callback
        self._logfile.BufferCallback = self._buffer_callback
        self._consumer.value = self._advapi32.OpenTraceW(ctypes.byref(self._logfile))
        if self._consumer.value == INVALID_PROCESSTRACE_HANDLE:
            raise ctypes.WinError(ctypes.get_last_error())
        status = int(self._advapi32.ProcessTrace(ctypes.byref(self._consumer), 1, None, None))
        if self._consumer.value != INVALID_PROCESSTRACE_HANDLE:
            self._advapi32.CloseTrace(self._consumer)
            self._consumer.value = INVALID_PROCESSTRACE_HANDLE
        if self._callback_error is not None:
            raise RuntimeError("ETW callback failed") from self._callback_error
        if status != ERROR_SUCCESS:
            self._raise_status("ProcessTrace", status)

    def losses(self) -> dict[str, int]:
        properties = self._properties.properties
        return {
            "events_lost": int(properties.EventsLost),
            "log_buffers_lost": int(properties.LogBuffersLost),
            "realtime_buffers_lost": int(properties.RealTimeBuffersLost),
            "consumer_events_lost": self._buffer_events_lost,
        }

    def _on_buffer(self, logfile: ctypes.POINTER(EVENT_TRACE_LOGFILEW)) -> int:
        if logfile:
            self._buffer_events_lost = max(self._buffer_events_lost, int(logfile.contents.EventsLost))
        return 1

    def _on_event(self, event_pointer: ctypes.POINTER(EVENT_RECORD)) -> None:
        try:
            event = event_pointer.contents
            payload = ctypes.string_at(event.UserData, event.UserDataLength) if event.UserDataLength else b""
            provider = event.EventHeader.ProviderId
            opcode = int(event.EventHeader.EventDescriptor.Opcode)
            ticks = int(event.EventHeader.TimeStamp)
            cpu = int(event.BufferContext.ProcessorIndex)
            if self._is_kernel_provider(provider, 0x3D6FA8D1):
                self._decode_thread(opcode, ticks, cpu, payload)
            elif self._is_kernel_provider(provider, 0x3D6FA8D0):
                pointer_size = 4 if event.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER else 8
                self._decode_process(opcode, ticks, cpu, pointer_size, payload)
        except BaseException as error:
            if self._callback_error is None:
                self._callback_error = error

    def _decode_thread(self, opcode: int, ticks: int, cpu: int, payload: bytes) -> None:
        if opcode == 36 and len(payload) >= 20:
            new_tid, old_tid = struct.unpack_from("<II", payload)
            new_priority, old_priority, previous_c_state = struct.unpack_from("<bbB", payload, 8)
            old_wait_reason, old_wait_mode, old_state, old_ideal = struct.unpack_from("<BBBb", payload, 12)
            new_wait_time, = struct.unpack_from("<I", payload, 16)
            if self._target_pid is None or old_tid in self._target_tids or new_tid in self._target_tids:
                self.context_switches.append((ticks, cpu, old_tid, new_tid, old_state, old_wait_reason,
                                              old_wait_mode, old_priority, new_priority, previous_c_state,
                                              old_ideal, new_wait_time))
        elif opcode == 50 and len(payload) >= 7:
            tid, adjustment, increment, flags = struct.unpack_from("<IbbB", payload)
            if self._target_pid is None or tid in self._target_tids:
                self.ready_events.append((ticks, cpu, tid, adjustment, increment, flags))
        elif opcode == 72 and len(payload) >= 10:
            process_id, thread_id = struct.unpack_from("<II", payload)
            name = payload[8:].decode("utf-16-le", errors="replace").split("\0", 1)[0]
            if self._target_pid is None or process_id == self._target_pid:
                self.thread_name_events.append((ticks, cpu, process_id, thread_id, name))
            if process_id == self._target_pid:
                self._target_tids.add(thread_id)
        elif opcode in (1, 2, 3) and len(payload) >= 8:
            process_id, thread_id = struct.unpack_from("<II", payload)
            kind = {1: "thread_create", 2: "thread_exit", 3: "thread_snapshot"}[opcode]
            if self._target_pid is None or process_id == self._target_pid:
                self.thread_events.append((kind, ticks, cpu, process_id, thread_id))
            if process_id == self._target_pid:
                self._target_tids.add(thread_id)

    def _decode_process(self, opcode: int, ticks: int, cpu: int, pointer_size: int, payload: bytes) -> None:
        if opcode not in (1, 2, 3) or len(payload) < pointer_size + 8:
            return
        key = int.from_bytes(payload[:pointer_size], "little")
        process_id, parent_id = struct.unpack_from("<II", payload, pointer_size)
        kind = {1: "process_create", 2: "process_exit", 3: "process_snapshot"}[opcode]
        if self._target_pid is None or process_id == self._target_pid:
            self.process_events.append((kind, ticks, cpu, process_id, parent_id, key))

    @staticmethod
    def _is_kernel_provider(provider: GUID, data1: int) -> bool:
        return (provider.Data1 == data1 and provider.Data2 == 0xFE05 and provider.Data3 == 0x11D0 and
                provider.Data4[0] == 0x9D and provider.Data4[1] == 0xDA and provider.Data4[2] == 0x00 and
                provider.Data4[3] == 0xC0 and provider.Data4[4] == 0x4F and provider.Data4[5] == 0xD7 and
                provider.Data4[6] == 0xBA and provider.Data4[7] == 0x7C)

    @staticmethod
    def _raise_status(operation: str, status: int) -> None:
        if status == ERROR_ACCESS_DENIED:
            raise PermissionError(f"{operation} requires an elevated administrator process (Windows error {status})")
        if status == ERROR_ALREADY_EXISTS:
            raise RuntimeError(f"{operation} could not acquire a system logger slot (Windows error {status})")
        raise OSError(status, f"{operation} failed: {ctypes.FormatError(status)}")


def capture(executable: Path, iterations: int, timeout: float, post_roll: float) -> dict:
    trace = KernelSchedulerTrace()
    etl_path = Path(tempfile.gettempdir()) / f"sora-spsc-{os.getpid()}-{time.time_ns()}.etl"
    trace.start(etl_path)
    capture_start = trace.qpc()
    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen([str(executable), "--iterations", str(iterations), "--trace-threads"])
        trace.set_target_pid(process.pid)
        return_code = process.wait(timeout=timeout)
        if return_code != 0:
            raise RuntimeError(f"SpscDemo returned exit code {return_code}")
        time.sleep(post_roll)
        capture_end = trace.qpc()
    except BaseException:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        trace.stop_collection()
        etl_path.unlink(missing_ok=True)
        raise
    trace.stop_collection()
    try:
        trace.consume()
    finally:
        etl_path.unlink(missing_ok=True)
    losses = trace.losses()
    target_pid = process.pid
    target_tids = sorted({event[4] for event in trace.thread_events if event[3] == target_pid})
    if not target_tids:
        raise RuntimeError("ETW did not report any thread lifecycle event for SpscDemo")

    events: list[dict] = []
    target_set = set(target_tids)
    starts = {tid: min(event[1] for event in trace.thread_events
                       if event[3] == target_pid and event[4] == tid and event[0] != "thread_exit")
              for tid in target_tids}
    ends = {tid: min((event[1] for event in trace.thread_events
                      if event[3] == target_pid and event[4] == tid and event[0] == "thread_exit"),
                     default=capture_end)
            for tid in target_tids}

    def active(thread_id: int, ticks: int) -> bool:
        return thread_id in target_set and starts[thread_id] <= ticks <= ends[thread_id]

    for kind, ticks, cpu, process_id, thread_id in trace.thread_events:
        if process_id == target_pid:
            events.append({"kind": kind, "ticks": ticks, "cpu": cpu, "pid": process_id, "tid": thread_id})
    for ticks, cpu, process_id, thread_id, name in trace.thread_name_events:
        if process_id == target_pid and active(thread_id, ticks):
            events.append({"kind": "thread_name", "ticks": ticks, "cpu": cpu, "pid": process_id,
                           "tid": thread_id, "name": name})
    for kind, ticks, cpu, process_id, parent_id, key in trace.process_events:
        if process_id == target_pid:
            events.append({"kind": kind, "ticks": ticks, "cpu": cpu, "pid": process_id,
                           "parent_pid": parent_id, "process_key": key})
    for item in trace.context_switches:
        ticks, cpu, old_tid, new_tid, state, reason, mode, old_priority, new_priority, c_state, ideal, wait = item
        if active(old_tid, ticks) or active(new_tid, ticks):
            events.append({"kind": "context_switch", "ticks": ticks, "cpu": cpu, "old_tid": old_tid,
                           "new_tid": new_tid, "old_state": state, "old_wait_reason": reason,
                           "old_wait_mode": mode, "old_priority": old_priority, "new_priority": new_priority,
                           "previous_c_state": c_state, "old_wait_ideal_processor": ideal,
                           "new_thread_wait_time": wait})
    for ticks, cpu, tid, adjustment, increment, flags in trace.ready_events:
        if active(tid, ticks):
            events.append({"kind": "thread_ready", "ticks": ticks, "cpu": cpu, "tid": tid,
                           "adjustment_reason": adjustment, "priority_increment": increment, "flags": flags})
    events.sort(key=lambda event: event["ticks"])
    names = {event["name"] for event in events if event["kind"] == "thread_name"}
    missing_roles = {"main", "producer", "consumer"} - names
    if missing_roles:
        raise RuntimeError(f"ETW did not capture SpscDemo thread names: {sorted(missing_roles)}")
    return {
        "schema_version": 1,
        "clock": {"kind": "QueryPerformanceCounter", "ticks_per_second": trace.ticks_per_second},
        "capture_start_ticks": capture_start,
        "capture_end_ticks": capture_end,
        "target_pid": target_pid,
        "target_tids": target_tids,
        "losses": losses,
        "events": events,
    }


def analyze(data: dict) -> dict:
    ticks_per_second = data["clock"]["ticks_per_second"]
    target_tids = set(data["target_tids"])
    events = sorted(data["events"], key=lambda event: event["ticks"])
    capture_start = data["capture_start_ticks"]
    capture_end = data["capture_end_ticks"]
    lifetimes = {tid: {"start": None, "end": None, "start_inferred": True, "end_inferred": True}
                 for tid in target_tids}
    running: dict[int, list[tuple[int, int, int]]] = {tid: [] for tid in target_tids}
    open_intervals: dict[int, tuple[int, int]] = {}
    switch_outs: list[dict] = []
    pending_ready: dict[int, int] = {}
    ready_latencies: list[dict] = []

    for event in events:
        kind = event["kind"]
        ticks = event["ticks"]
        if kind in ("thread_create", "thread_snapshot"):
            lifetime = lifetimes[event["tid"]]
            if lifetime["start"] is None or ticks < lifetime["start"]:
                lifetime["start"] = ticks
                lifetime["start_inferred"] = kind != "thread_create"
        elif kind == "thread_exit":
            lifetime = lifetimes[event["tid"]]
            lifetime["end"] = ticks
            lifetime["end_inferred"] = False
        elif kind == "thread_ready":
            pending_ready[event["tid"]] = ticks
        elif kind == "context_switch":
            old_tid = event["old_tid"]
            new_tid = event["new_tid"]
            if old_tid in target_tids:
                start, cpu = open_intervals.pop(old_tid, (max(capture_start, ticks), event["cpu"]))
                if ticks > start:
                    running[old_tid].append((start, ticks, cpu))
                switch_outs.append(event)
            if new_tid in target_tids:
                previous = open_intervals.pop(new_tid, None)
                if previous is not None and ticks > previous[0]:
                    running[new_tid].append((previous[0], ticks, previous[1]))
                open_intervals[new_tid] = (ticks, event["cpu"])
                ready_ticks = pending_ready.pop(new_tid, None)
                if ready_ticks is not None and ready_ticks <= ticks:
                    ready_latencies.append({"tid": new_tid, "ticks": ticks, "ready_ticks": ready_ticks,
                                            "latency_us": (ticks - ready_ticks) * 1_000_000 / ticks_per_second})

    for tid, (start, cpu) in open_intervals.items():
        finish = lifetimes[tid]["end"] if lifetimes[tid]["end"] is not None else capture_end
        if finish > start:
            running[tid].append((start, finish, cpu))
    for lifetime in lifetimes.values():
        if lifetime["start"] is None:
            lifetime["start"] = capture_start
        if lifetime["end"] is None:
            lifetime["end"] = capture_end
    return {"lifetimes": lifetimes, "running": running, "switch_outs": switch_outs,
            "ready_latencies": ready_latencies}


def thread_labels(data: dict) -> dict[int, str]:
    names = {event["tid"]: event["name"] for event in data["events"] if event["kind"] == "thread_name"}
    creates = [event for event in data["events"] if event["kind"] == "thread_create"]
    creates.sort(key=lambda event: event["ticks"])
    ordered = []
    for event in creates:
        if event["tid"] not in ordered:
            ordered.append(event["tid"])
    for tid in data["target_tids"]:
        if tid not in ordered:
            ordered.append(tid)
    return {tid: f"{names.get(tid, 'thread')} ({tid})" for tid in ordered}


def plot(data: dict, output: Path) -> None:
    try:
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D
    except ImportError as error:
        raise RuntimeError("matplotlib is required: python -m pip install matplotlib") from error

    analysis = analyze(data)
    labels = thread_labels(data)
    tids = list(labels)
    y_for_tid = {tid: index for index, tid in enumerate(tids)}
    origin = min(lifetime["start"] for lifetime in analysis["lifetimes"].values())
    frequency = data["clock"]["ticks_per_second"]
    to_ms = lambda ticks: (ticks - origin) * 1000 / frequency
    cpu_values = sorted({segment[2] for segments in analysis["running"].values() for segment in segments})
    cmap = plt.colormaps["tab20"]
    cpu_colors = {cpu: cmap(index % 20) for index, cpu in enumerate(cpu_values)}

    figure, axes = plt.subplots(3, 1, figsize=(15, 9), sharex=True,
                                gridspec_kw={"height_ratios": [2.3, 1.6, 1.4]}, constrained_layout=True)
    lifetime_axis, reason_axis, latency_axis = axes
    for tid in tids:
        y = y_for_tid[tid]
        lifetime = analysis["lifetimes"][tid]
        start = to_ms(lifetime["start"])
        end = to_ms(lifetime["end"])
        lifetime_axis.hlines(y, start, end, color="#a8b0b8", linewidth=2, zorder=1)
        lifetime_axis.scatter([start, end], [y, y], marker="|", s=100,
                              color=["#c0392b" if lifetime["start_inferred"] else "#20262c",
                                     "#c0392b" if lifetime["end_inferred"] else "#20262c"], zorder=3)
        bars = [(to_ms(begin), (finish - begin) * 1000 / frequency)
                for begin, finish, _ in analysis["running"][tid]]
        colors = [cpu_colors[cpu] for _, _, cpu in analysis["running"][tid]]
        if bars:
            lifetime_axis.broken_barh(bars, (y - 0.31, 0.62), facecolors=colors, edgecolors="none", zorder=2)
    lifetime_axis.set_yticks(range(len(tids)), [labels[tid] for tid in tids])
    lifetime_axis.set_ylabel("Thread lifetime")
    lifetime_axis.set_title(f"SpscDemo scheduler trace, PID {data['target_pid']}")
    lifetime_axis.grid(axis="x", color="#d9dde1", linewidth=0.6)

    reasons = sorted({event["old_wait_reason"] for event in analysis["switch_outs"]})
    reason_colors = {reason: cmap(index % 20) for index, reason in enumerate(reasons)}
    for event in analysis["switch_outs"]:
        tid = event["old_tid"]
        reason_axis.scatter(to_ms(event["ticks"]), y_for_tid[tid], s=24,
                            color=reason_colors[event["old_wait_reason"]], marker="o")
    reason_axis.set_yticks(range(len(tids)), [labels[tid] for tid in tids])
    reason_axis.set_ylabel("Switch-out")
    reason_axis.grid(axis="x", color="#d9dde1", linewidth=0.6)
    if reasons:
        handles = []
        for reason in reasons:
            states = sorted({event["old_state"] for event in analysis["switch_outs"]
                             if event["old_wait_reason"] == reason})
            state_text = "/".join(THREAD_STATES.get(state, f"State({state})") for state in states)
            reason_text = WAIT_REASONS.get(reason, f"WaitReason({reason})")
            handles.append(Line2D([], [], marker="o", linestyle="", color=reason_colors[reason],
                                  label=f"{reason_text}; state={state_text}"))
        reason_axis.legend(handles=handles, loc="upper left", bbox_to_anchor=(1.005, 1), fontsize=8,
                           borderaxespad=0)

    if cpu_values:
        cpu_handles = [Line2D([], [], color=cpu_colors[cpu], linewidth=7, label=f"CPU {cpu}")
                       for cpu in cpu_values]
        lifetime_axis.legend(handles=cpu_handles, loc="upper left", bbox_to_anchor=(1.005, 1), fontsize=8,
                             borderaxespad=0, ncols=max(1, (len(cpu_handles) + 9) // 10))

    latency_by_tid = {tid: [] for tid in tids}
    for point in analysis["ready_latencies"]:
        latency_by_tid[point["tid"]].append(point)
    for index, tid in enumerate(tids):
        points = latency_by_tid[tid]
        if points:
            latency_axis.scatter([to_ms(point["ticks"]) for point in points],
                                 [point["latency_us"] for point in points], s=24,
                                 color=cmap(index % 20), label=labels[tid])
    latency_axis.set_ylabel("Ready latency (us)")
    latency_axis.set_xlabel("Time since first target-thread lifetime event (ms)")
    latency_axis.set_yscale("symlog", linthresh=1)
    latency_axis.grid(color="#d9dde1", linewidth=0.6)
    if any(latency_by_tid.values()):
        latency_axis.legend(loc="upper right", fontsize=8)

    losses = data["losses"]
    loss_total = sum(losses.values())
    figure.suptitle("ETW loss counters: " + ("zero" if loss_total == 0 else json.dumps(losses)),
                    color="#20262c" if loss_total == 0 else "#c0392b", fontsize=10)
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, dpi=170)
    plt.close(figure)


def find_executable(repo_root: Path) -> Path:
    candidates = [
        repo_root / "build/x64-release/bin/Demo.Sora.Core.SpscDemo.exe",
        repo_root / "build/x64-debug/bin/Demo.Sora.Core.SpscDemo.exe",
        repo_root / "build/x64-asan/bin/Demo.Sora.Core.SpscDemo.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("SpscDemo was not found; pass --exe or build Demo.Sora.Core.SpscDemo")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, help="path to Demo.Sora.Core.SpscDemo.exe")
    parser.add_argument("--iterations", type=int, default=1_000_000)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--post-roll", type=float, default=0.1)
    parser.add_argument("--json", type=Path, default=Path("spsc_scheduler_trace.json"))
    parser.add_argument("--output", type=Path, default=Path("spsc_scheduler_trace.png"))
    parser.add_argument("--input-json", type=Path, help="render an existing capture without running ETW")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.input_json:
        data = json.loads(args.input_json.read_text(encoding="utf-8"))
    else:
        if os.name != "nt":
            raise RuntimeError("Live kernel scheduler tracing is available only on Windows")
        if args.iterations <= 0:
            raise ValueError("--iterations must be positive")
        repo_root = Path(__file__).resolve().parents[2]
        executable = args.exe.resolve() if args.exe else find_executable(repo_root)
        data = capture(executable, args.iterations, args.timeout, args.post_roll)
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(data, indent=2), encoding="utf-8")
    loss_total = sum(data["losses"].values())
    if loss_total:
        raise RuntimeError(f"ETW reported lost events; refusing to present incomplete timing data: {data['losses']}")
    plot(data, args.output)
    print(f"Trace: {args.json if not args.input_json else args.input_json}")
    print(f"Plot:  {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
