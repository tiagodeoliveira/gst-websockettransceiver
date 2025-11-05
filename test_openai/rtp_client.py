#!/usr/bin/env python3
"""
RTP audio test client - sends/receives G.711 μ-law audio via RTP.
"""

import socket
import pyaudio
import audioop
import time
import threading
import argparse
import random
from rtp import RTP, PayloadType

# Audio config
RATE = 8000
CHUNK = 160  # 20ms at 8kHz
RTP_PAYLOAD_TYPE = 0  # PCMU

# Defaults (matches standalone_pipeline.py)
DEFAULT_LOCAL_PORT = 5000
DEFAULT_REMOTE_IP = "127.0.0.1"
DEFAULT_REMOTE_PORT = 5060


class RTPClient:
    def __init__(self, local_port, remote_ip, remote_port):
        self.local_port = local_port
        self.remote_ip = remote_ip
        self.remote_port = remote_port

        # RTP state
        self.ssrc = random.randint(0, 0xFFFFFFFF)
        self.sequence_number = random.randint(0, 65535)
        self.timestamp = random.randint(0, 0xFFFFFFFF)

        # UDP sockets
        self.send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.recv_sock.bind(("0.0.0.0", local_port))

        # PyAudio
        self.audio = pyaudio.PyAudio()
        self.stream = self.audio.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=RATE,
            input=True,
            output=True,
            frames_per_buffer=CHUNK,
        )

        self.running = True
        self.packets_sent = 0
        self.packets_received = 0

    def parse_rtp_packet(self, packet_data):
        """Parse RTP packet using BBC rtp library"""
        try:
            packet = RTP()
            packet.fromBytes(packet_data)
            return bytes(packet.payload), packet.sequenceNumber, packet.timestamp
        except Exception:
            return None, None, None

    def send_loop(self):
        """Read from mic, encode to G.711, send via RTP"""
        while self.running:
            try:
                pcm_data = self.stream.read(CHUNK, exception_on_overflow=False)
                ulaw_data = audioop.lin2ulaw(pcm_data, 2)

                packet = RTP(
                    payloadType=PayloadType.PCMU,
                    sequenceNumber=self.sequence_number,
                    timestamp=self.timestamp,
                    ssrc=self.ssrc,
                    payload=bytearray(ulaw_data),
                )

                self.send_sock.sendto(
                    packet.toBytes(), (self.remote_ip, self.remote_port)
                )

                self.sequence_number = (self.sequence_number + 1) & 0xFFFF
                self.timestamp = (self.timestamp + CHUNK) & 0xFFFFFFFF
                self.packets_sent += 1

            except Exception as e:
                print(f"Send error: {e}")
                time.sleep(0.01)

    def recv_loop(self):
        """Receive RTP, decode G.711, play to speaker"""
        self.recv_sock.settimeout(0.1)
        last_seq = None

        while self.running:
            try:
                packet, addr = self.recv_sock.recvfrom(2048)
                ulaw_data, seq_num, rtp_timestamp = self.parse_rtp_packet(packet)
                if not ulaw_data:
                    continue

                self.packets_received += 1

                # Check for sequence gaps
                if last_seq is not None:
                    expected_seq = (last_seq + 1) & 0xFFFF
                    if seq_num != expected_seq:
                        gap = (seq_num - expected_seq) & 0xFFFF
                        print(
                            f"⚠️  Packet loss: expected seq {expected_seq}, got {seq_num} (gap={gap})"
                        )

                last_seq = seq_num

                # Decode and play
                pcm_data = audioop.ulaw2lin(ulaw_data, 2)
                self.stream.write(pcm_data)

            except socket.timeout:
                continue
            except Exception as e:
                print(f"Recv error: {e}")
                time.sleep(0.01)

    def start(self):
        """Start send and receive threads"""
        send_thread = threading.Thread(target=self.send_loop, daemon=True)
        recv_thread = threading.Thread(target=self.recv_loop, daemon=True)

        send_thread.start()
        recv_thread.start()

        print(
            f"RTP client running: {self.local_port} <-> {self.remote_ip}:{self.remote_port}"
        )
        print("Press Ctrl+C to stop...")

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nStopping...")
            self.running = False
            send_thread.join(timeout=1)
            recv_thread.join(timeout=1)
            print(f"Sent: {self.packets_sent}, Received: {self.packets_received}")

    def cleanup(self):
        """Clean up resources"""
        self.stream.stop_stream()
        self.stream.close()
        self.audio.terminate()
        self.send_sock.close()
        self.recv_sock.close()


def main():
    parser = argparse.ArgumentParser(description="RTP audio test client")
    parser.add_argument(
        "--local-port",
        type=int,
        default=DEFAULT_LOCAL_PORT,
        help=f"Local port (default: {DEFAULT_LOCAL_PORT})",
    )
    parser.add_argument(
        "--remote-ip",
        default=DEFAULT_REMOTE_IP,
        help=f"Remote IP (default: {DEFAULT_REMOTE_IP})",
    )
    parser.add_argument(
        "--remote-port",
        type=int,
        default=DEFAULT_REMOTE_PORT,
        help=f"Remote port (default: {DEFAULT_REMOTE_PORT})",
    )
    args = parser.parse_args()

    client = RTPClient(args.local_port, args.remote_ip, args.remote_port)

    try:
        client.start()
    finally:
        client.cleanup()


if __name__ == "__main__":
    main()
