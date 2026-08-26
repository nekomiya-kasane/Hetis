import importlib.util
from pathlib import Path
import struct
import unittest
import uuid


SCRIPT = Path(__file__).with_name("plot_spsc_scheduler.py")
SPEC = importlib.util.spec_from_file_location("plot_spsc_scheduler", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class SchedulerAnalysisTest(unittest.TestCase):
    def test_reconstructs_running_intervals_and_ready_latency(self):
        data = {
            "clock": {"kind": "QueryPerformanceCounter", "ticks_per_second": 1_000_000},
            "capture_start_ticks": 90,
            "capture_end_ticks": 500,
            "target_pid": 7,
            "target_tids": [11, 12],
            "losses": {"events_lost": 0, "log_buffers_lost": 0, "realtime_buffers_lost": 0,
                       "consumer_events_lost": 0},
            "events": [
                {"kind": "thread_create", "ticks": 100, "cpu": 0, "pid": 7, "tid": 11},
                {"kind": "thread_create", "ticks": 110, "cpu": 1, "pid": 7, "tid": 12},
                {"kind": "thread_ready", "ticks": 120, "cpu": 1, "tid": 12},
                {"kind": "context_switch", "ticks": 150, "cpu": 1, "old_tid": 99, "new_tid": 12,
                 "old_state": 5, "old_wait_reason": 6},
                {"kind": "context_switch", "ticks": 200, "cpu": 0, "old_tid": 98, "new_tid": 11,
                 "old_state": 5, "old_wait_reason": 6},
                {"kind": "context_switch", "ticks": 250, "cpu": 1, "old_tid": 12, "new_tid": 99,
                 "old_state": 5, "old_wait_reason": 33},
                {"kind": "thread_exit", "ticks": 300, "cpu": 1, "pid": 7, "tid": 12},
                {"kind": "thread_exit", "ticks": 400, "cpu": 0, "pid": 7, "tid": 11},
            ],
        }

        result = MODULE.analyze(data)

        self.assertEqual(result["running"][12], [(150, 250, 1)])
        self.assertEqual(result["running"][11], [(200, 400, 0)])
        self.assertEqual(result["ready_latencies"], [
            {"tid": 12, "ticks": 150, "ready_ticks": 120, "latency_us": 30.0}
        ])
        self.assertEqual(result["lifetimes"][12]["start"], 110)
        self.assertEqual(result["lifetimes"][12]["end"], 300)
        self.assertFalse(result["lifetimes"][12]["start_inferred"])
        self.assertFalse(result["lifetimes"][12]["end_inferred"])

    def test_labels_threads_by_creation_order(self):
        data = {
            "target_tids": [30, 10, 20],
            "events": [
                {"kind": "thread_create", "ticks": 3, "tid": 20},
                {"kind": "thread_create", "ticks": 1, "tid": 10},
                {"kind": "thread_create", "ticks": 2, "tid": 30},
                {"kind": "thread_name", "ticks": 4, "tid": 10, "name": "main"},
                {"kind": "thread_name", "ticks": 5, "tid": 30, "name": "producer"},
                {"kind": "thread_name", "ticks": 6, "tid": 20, "name": "consumer"},
            ],
        }

        labels = MODULE.thread_labels(data)

        self.assertEqual(labels[10], "main (10)")
        self.assertEqual(labels[30], "producer (30)")
        self.assertEqual(labels[20], "consumer (20)")

    def test_callback_filter_keeps_only_target_scheduler_events(self):
        trace = MODULE.KernelSchedulerTrace()
        trace.set_target_pid(7)
        trace._decode_thread(1, 100, 0, struct.pack("<II", 7, 11))
        unrelated_switch = struct.pack("<IIbbBxBBBbI", 21, 20, 8, 8, 0, 6, 1, 5, 0, 0)
        target_switch = struct.pack("<IIbbBxBBBbI", 11, 20, 8, 8, 0, 6, 1, 5, 0, 0)

        trace._decode_thread(36, 110, 0, unrelated_switch)
        trace._decode_thread(36, 120, 0, target_switch)

        self.assertEqual(len(trace.context_switches), 1)
        self.assertEqual(trace.context_switches[0][3], 11)

    def test_kernel_provider_comparison_uses_complete_guid(self):
        thread_guid = MODULE.GUID.from_uuid(uuid.UUID("3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c"))
        wrong_tail = MODULE.GUID.from_uuid(uuid.UUID("3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7d"))

        self.assertTrue(MODULE.KernelSchedulerTrace._is_kernel_provider(thread_guid, 0x3D6FA8D1))
        self.assertFalse(MODULE.KernelSchedulerTrace._is_kernel_provider(wrong_tail, 0x3D6FA8D1))


if __name__ == "__main__":
    unittest.main()
