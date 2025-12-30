#!/usr/bin/env python3
"""
GStreamer RTP-to-WebSocket Bridge

Bridges RTP L16 (linear PCM) audio to/from WebSocket.
Audio format: PCM 16-bit @ 24kHz mono
"""

import argparse
import logging
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

DEFAULT_RTP_PORT = 5060
DEFAULT_CLIENT_RTP_PORT = 10000
DEFAULT_WS_URI = "ws://localhost:8765"

SAMPLE_RATE = 24000
CHANNELS = 1


def setup_logging(debug: bool = False):
    level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s - %(levelname)s - %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    return logging.getLogger(__name__)


class RTPWebSocketBridge:
    """Bridge between RTP and WebSocket using GStreamer."""

    def __init__(self, rtp_port, client_ip, client_rtp_port, ws_uri, debug=False):
        self.rtp_port = rtp_port
        self.client_ip = client_ip
        self.client_rtp_port = client_rtp_port
        self.ws_uri = ws_uri
        self.debug = debug
        self.logger = logging.getLogger("RTPWebSocketBridge")
        self.pipeline = None
        self.loop = None

    def on_bus_message(self, bus, message):
        t = message.type

        if t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            self.logger.error(f"Pipeline error: {err.message}")
            if debug and self.debug:
                self.logger.debug(f"Debug: {debug}")
            self.loop.quit()

        elif t == Gst.MessageType.WARNING:
            warn, debug = message.parse_warning()
            self.logger.warning(f"Pipeline warning: {warn.message}")

        elif t == Gst.MessageType.EOS:
            self.logger.info("End of stream")
            self.loop.quit()

        elif t == Gst.MessageType.STATE_CHANGED:
            if message.src == self.pipeline:
                old, new, _ = message.parse_state_changed()
                self.logger.info(f"Pipeline: {old.value_nick} -> {new.value_nick}")

        return True

    def create_pipeline(self):
        """Create the GStreamer pipeline.

        RTP L16 @ 24kHz <-> WebSocket (raw PCM binary)
        """
        input_caps = (
            f"application/x-rtp,media=audio,clock-rate={SAMPLE_RATE},"
            f"encoding-name=L16,channels={CHANNELS},payload=96"
        )

        # websockettransceiver outputs S16LE, rtpL16pay needs S16BE (network order)
        pipeline_desc = f"""
            udpsrc port={self.rtp_port} caps="{input_caps}"
            ! rtpL16depay
            ! audioconvert
            ! audio/x-raw,format=S16LE,rate={SAMPLE_RATE},channels={CHANNELS},layout=interleaved
            ! websockettransceiver name=ws uri={self.ws_uri}
                sample-rate={SAMPLE_RATE}
                channels={CHANNELS}
                max-queue-size=50
                initial-buffer-count=2
                frame-duration-ms=20
            ! queue leaky=downstream max-size-buffers=100 max-size-time=0 max-size-bytes=0
            ! audioconvert
            ! audio/x-raw,format=S16BE,rate={SAMPLE_RATE},channels={CHANNELS},layout=interleaved
            ! rtpL16pay min-ptime=20000000 max-ptime=20000000 pt=96
            ! udpsink host={self.client_ip} port={self.client_rtp_port} sync=false async=false
        """

        try:
            self.pipeline = Gst.parse_launch(pipeline_desc)
            bus = self.pipeline.get_bus()
            bus.add_signal_watch()
            bus.connect("message", self.on_bus_message)
            return True
        except Exception as e:
            self.logger.error(f"Failed to create pipeline: {e}")
            return False

    def start(self):
        self.logger.info("=" * 60)
        self.logger.info("RTP-to-WebSocket Bridge")
        self.logger.info("=" * 60)
        self.logger.info(f"RTP listen:    {self.rtp_port}")
        self.logger.info(f"WebSocket:     {self.ws_uri}")
        self.logger.info(f"Client RTP:    {self.client_ip}:{self.client_rtp_port}")
        self.logger.info(f"Audio:         L16 PCM @ {SAMPLE_RATE}Hz mono")
        self.logger.info("=" * 60)

        if not self.create_pipeline():
            return False

        self.loop = GLib.MainLoop()

        self.logger.info("Setting pipeline to PLAYING...")
        ret = self.pipeline.set_state(Gst.State.PLAYING)

        if ret == Gst.StateChangeReturn.FAILURE:
            self.logger.error("Failed to start pipeline")
            return False

        self.logger.info("Bridge running. Ctrl+C to stop.")

        try:
            self.loop.run()
        except KeyboardInterrupt:
            self.logger.info("\nStopping...")

        self.pipeline.set_state(Gst.State.NULL)
        return True


def main():
    parser = argparse.ArgumentParser(description="RTP-to-WebSocket bridge")
    parser.add_argument("--rtp-port", type=int, default=DEFAULT_RTP_PORT)
    parser.add_argument("--client-ip", default="127.0.0.1")
    parser.add_argument("--client-rtp-port", type=int, default=DEFAULT_CLIENT_RTP_PORT)
    parser.add_argument("--ws-uri", default=DEFAULT_WS_URI)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--gst-debug", help="GStreamer debug (e.g., 'websockettransceiver:5')")

    args = parser.parse_args()

    setup_logging(args.debug)
    Gst.init(None)

    if args.gst_debug:
        if ":" in args.gst_debug:
            Gst.debug_set_threshold_from_string(args.gst_debug, True)
        else:
            Gst.debug_set_default_threshold(int(args.gst_debug))

    if not Gst.Registry.get().find_plugin("websockettransceiver"):
        logging.error("websockettransceiver plugin not found!")
        logging.error("Set GST_PLUGIN_PATH to build/src")
        sys.exit(1)

    bridge = RTPWebSocketBridge(
        rtp_port=args.rtp_port,
        client_ip=args.client_ip,
        client_rtp_port=args.client_rtp_port,
        ws_uri=args.ws_uri,
        debug=args.debug,
    )

    sys.exit(0 if bridge.start() else 1)


if __name__ == "__main__":
    main()
