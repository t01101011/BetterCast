#!/usr/bin/env python3
"""Static regression contracts for the Windows sender's cross-thread pipeline."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SENDER = ROOT / "Sources/BetterCastReceiverDesktop/sender"


class WindowsLowLatencyContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.controller_h = (SENDER / "SenderController.h").read_text()
        cls.controller_cpp = (SENDER / "SenderController.cpp").read_text()
        cls.encoder_h = (SENDER / "VideoEncoderFF.h").read_text()
        cls.network_cpp = (SENDER / "NetworkSender.cpp").read_text()

    def test_keyframe_request_is_atomic(self):
        self.assertRegex(
            self.encoder_h,
            r"std::atomic<bool>\s+m_forceKeyframe",
            "requestKeyframe crosses GUI/capture threads and must not race",
        )

    def test_encoded_frames_use_one_slot_mailbox_not_queued_payload_events(self):
        self.assertIn("pendingPayload", self.controller_h)
        self.assertIn("networkDrainScheduled", self.controller_h)
        self.assertNotRegex(
            self.controller_cpp,
            r"VideoEncoderFF::encoded[\s\S]{0,400}Qt::QueuedConnection",
            "queued QByteArray signals create an unbounded stale-frame FIFO",
        )

    def test_pending_keyframe_is_not_replaced_by_delta_frame(self):
        self.assertRegex(
            self.controller_cpp,
            r"pendingKeyframe\s*&&\s*!keyframe",
            "a queued IDR must survive later delta frames until GUI drain",
        )

    def test_pending_delivery_does_not_capture_raw_session_pointer(self):
        self.assertIn("QPointer<NetworkSender>", self.controller_h)
        self.assertNotRegex(
            self.controller_cpp,
            r"invokeMethod\([^\n]*\[this,\s*s\]",
            "posted callbacks must survive Session destruction safely",
        )

    def test_tcp_low_delay_is_applied_after_connection(self):
        connected = re.search(
            r"QTcpSocket::connected[\s\S]*?\}\);", self.network_cpp
        )
        self.assertIsNotNone(connected)
        assert connected is not None
        self.assertIn("LowDelayOption", connected.group(0))

    def test_empty_socket_may_accept_one_oversized_keyframe(self):
        self.assertRegex(
            self.network_cpp,
            r"queued\s*>\s*0\s*&&\s*queued\s*\+\s*packetBytes\s*>\s*MaxQueuedVideoBytes",
            "an IDR larger than the backlog budget must not livelock recovery",
        )

    def test_vdd_shutdown_detaches_monitors_without_removing_device_nodes(self):
        vdd_h = (SENDER / "VirtualDisplayVDD.h").read_text()
        vdd_cpp = (SENDER / "VirtualDisplayVDD.cpp").read_text()
        self.assertIn("detachAllVirtualDisplays", vdd_h)
        destructor = re.search(
            r"VirtualDisplayVDD::~VirtualDisplayVDD\(\)[\s\S]*?\n}", vdd_cpp
        )
        self.assertIsNotNone(destructor)
        assert destructor is not None
        self.assertIn("detachAllVirtualDisplays()", destructor.group(0))
        self.assertNotIn("removeVddDevice", destructor.group(0))
        self.assertIn("mon.isManagedVdd", vdd_cpp)

    def test_first_send_prepares_only_one_virtual_display(self):
        self.assertRegex(
            self.controller_h,
            r"kDisplayPoolSize\s*=\s*1\s*;",
            "pre-creating three nodes exposes two idle 800x600 displays",
        )


if __name__ == "__main__":
    unittest.main()
