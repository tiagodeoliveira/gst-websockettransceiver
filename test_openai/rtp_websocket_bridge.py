#!/usr/bin/env python3
"""
GStreamer RTP-to-WebSocket Bridge

Bridges RTP G.711 μ-law audio to/from WebSocket using the websockettransceiver plugin.
"""

import argparse
import logging
import sys

import gi
from gi.repository import GLib, Gst

gi.require_version("Gst", "1.0")


# Configuration
DEFAULT_RTP_PORT = 5060
DEFAULT_CLIENT_RTP_PORT = 10000
DEFAULT_WS_URI = "ws://localhost:8765"
AUDIO_RATE = 8000
AUDIO_CHANNELS = 1


def setup_logging(debug: bool = False):
    """Setup logging configuration."""
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
        """Initialize the bridge.

        Parameters:
            rtp_port: Port to listen for RTP from client
            client_ip: IP address of the RTP client
            client_rtp_port: Port to send RTP back to client
            ws_uri: WebSocket URI to connect to
            debug: Enable debug logging
        """
        self.rtp_port = rtp_port
        self.client_ip = client_ip
        self.client_rtp_port = client_rtp_port
        self.ws_uri = ws_uri
        self.debug = debug
        self.logger = logging.getLogger("RTPWebSocketBridge")

        self.pipeline = None
        self.loop = None

    def on_bus_message(self, bus, message):
        """Handle GStreamer bus messages."""
        t = message.type

        if t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            self.logger.error(f"Pipeline error: {err.message}")
            if debug and self.debug:
                self.logger.debug(f"Debug info: {debug}")
            self.loop.quit()

        elif t == Gst.MessageType.WARNING:
            warn, debug = message.parse_warning()
            self.logger.warning(f"Pipeline warning: {warn.message}")
            if debug and self.debug:
                self.logger.debug(f"Debug info: {debug}")

        elif t == Gst.MessageType.EOS:
            self.logger.info("End of stream")
            self.loop.quit()

        elif t == Gst.MessageType.STATE_CHANGED:
            if message.src == self.pipeline:
                old_state, new_state, pending = message.parse_state_changed()
                self.logger.info(
                    f"Pipeline: {old_state.value_nick} -> {new_state.value_nick}"
                )

        return True

    def create_pipeline(self):
        """Create the GStreamer pipeline."""
        pipeline_desc = f"""
            udpsrc port={self.rtp_port} caps="application/x-rtp,media=audio,clock-rate=8000,encoding-name=PCMU,payload=0"
            ! rtppcmudepay
            ! websockettransceiver name=ws uri={self.ws_uri} max-queue-size=50 initial-buffer-count=3 frame-duration-ms=20
            ! rtppcmupay min-ptime=20000000 max-ptime=20000000
            ! udpsink host={self.client_ip} port={self.client_rtp_port} sync=false
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
        """Start the bridge server."""
        self.logger.info("=" * 60)
        self.logger.info("RTP-to-WebSocket Bridge Server")
        self.logger.info("=" * 60)
        self.logger.info(f"RTP listen:      {self.rtp_port}")
        self.logger.info(f"WebSocket URI:   {self.ws_uri}")
        self.logger.info(f"Client RTP:      {self.client_ip}:{self.client_rtp_port}")
        self.logger.info("=" * 60)

        if not self.create_pipeline():
            return False

        self.loop = GLib.MainLoop()
        ret = self.pipeline.set_state(Gst.State.PLAYING)

        if ret == Gst.StateChangeReturn.FAILURE:
            self.logger.error("Unable to set pipeline to PLAYING state")
            return False

        self.logger.info("Bridge running. Press Ctrl+C to stop.")

        try:
            self.loop.run()
        except KeyboardInterrupt:
            self.logger.info("\nStopping...")

        self.pipeline.set_state(Gst.State.NULL)
        self.logger.info("Stopped")

        return True


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="GStreamer RTP-to-WebSocket bridge server"
    )
    parser.add_argument(
        "--rtp-port",
        type=int,
        default=DEFAULT_RTP_PORT,
        help=f"Port to listen for RTP from client (default: {DEFAULT_RTP_PORT})",
    )
    parser.add_argument(
        "--client-ip",
        default="127.0.0.1",
        help="IP address of the RTP client (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--client-rtp-port",
        type=int,
        default=DEFAULT_CLIENT_RTP_PORT,
        help=f"Port to send RTP back to client (default: {DEFAULT_CLIENT_RTP_PORT})",
    )
    parser.add_argument(
        "--ws-uri",
        default=DEFAULT_WS_URI,
        help=f"WebSocket URI to connect to (default: {DEFAULT_WS_URI})",
    )
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    parser.add_argument(
        "--gst-debug",
        help="GStreamer debug level (e.g., '5' or 'websockettransceiver:6')",
    )

    args = parser.parse_args()

    logger = setup_logging(args.debug)
    Gst.init(None)

    if args.gst_debug:
        if ":" in args.gst_debug:
            Gst.debug_set_threshold_from_string(args.gst_debug, True)
        else:
            Gst.debug_set_default_threshold(int(args.gst_debug))

    if not Gst.Registry.get().find_plugin("websockettransceiver"):
        logger.error("websockettransceiver plugin not found!")
        logger.error("Set GST_PLUGIN_PATH: export GST_PLUGIN_PATH=/path/to/build/src")
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
