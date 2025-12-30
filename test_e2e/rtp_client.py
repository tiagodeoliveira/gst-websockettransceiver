#!/usr/bin/env python3
"""
RTP audio client - sends/receives L16 (linear PCM) audio via RTP.
Audio format: PCM 16-bit @ 24kHz mono
"""

import socket
import pyaudio
import time
import threading
import argparse
import random
import struct
from rtp import RTP, PayloadType

SAMPLE_RATE = 24000
CHUNK = 480  # 20ms at 24kHz

DEFAULT_LOCAL_PORT = 10000
DEFAULT_REMOTE_IP = "127.0.0.1"
DEFAULT_REMOTE_PORT = 5060


class RTPClient:
    def __init__(self, local_port, remote_ip, remote_port):
        self.local_port = local_port
        self.remote_ip = remote_ip
        self.remote_port = remote_port

        self.ssrc = random.randint(0, 0xFFFFFFFF)
        self.sequence_number = random.randint(0, 65535)
        self.timestamp = random.randint(0, 0xFFFFFFFF)

        self.send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.recv_sock.bind(("0.0.0.0", local_port))

        self.audio = pyaudio.PyAudio()
        self.stream = self.audio.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=SAMPLE_RATE,
            input=True,
            output=True,
            frames_per_buffer=CHUNK,
        )

        self.running = True
        self.packets_sent = 0
        self.packets_received = 0

    def pcm_to_network(self, pcm_data: bytes) -> bytes:
        """Convert PCM from little-endian to big-endian (network order)."""
        samples = struct.unpack(f"<{len(pcm_data)//2}h", pcm_data)
        return struct.pack(f">{len(samples)}h", *samples)

    def network_to_pcm(self, network_data: bytes) -> bytes:
        """Convert PCM from big-endian (network order) to little-endian."""
        samples = struct.unpack(f">{len(network_data)//2}h", network_data)
        return struct.pack(f"<{len(samples)}h", *samples)

    def parse_rtp(self, packet_data):
        try:
            packet = RTP()
            packet.fromBytes(packet_data)
            return bytes(packet.payload), packet.sequenceNumber
        except:
            return None, None

    def send_loop(self):
        while self.running:
            try:
                pcm_data = self.stream.read(CHUNK, exception_on_overflow=False)
                network_data = self.pcm_to_network(pcm_data)

                packet = RTP(
                    payloadType=PayloadType.DYNAMIC_96,
                    sequenceNumber=self.sequence_number,
                    timestamp=self.timestamp,
                    ssrc=self.ssrc,
                    payload=bytearray(network_data),
                )

                self.send_sock.sendto(packet.toBytes(), (self.remote_ip, self.remote_port))
                self.sequence_number = (self.sequence_number + 1) & 0xFFFF
                self.timestamp = (self.timestamp + CHUNK) & 0xFFFFFFFF
                self.packets_sent += 1

            except Exception as e:
                print(f"Send error: {e}")
                time.sleep(0.01)

    def recv_loop(self):
        self.recv_sock.settimeout(0.1)
        last_seq = None

        while self.running:
            try:
                packet, _ = self.recv_sock.recvfrom(4096)
                network_data, seq_num = self.parse_rtp(packet)
                if not network_data:
                    continue

                self.packets_received += 1

                if last_seq is not None:
                    expected = (last_seq + 1) & 0xFFFF
                    if seq_num != expected:
                        gap = (seq_num - expected) & 0xFFFF
                        print(f"Packet loss: gap={gap}")

                last_seq = seq_num
                pcm_data = self.network_to_pcm(network_data)
                self.stream.write(pcm_data)

            except socket.timeout:
                continue
            except Exception as e:
                print(f"Recv error: {e}")
                time.sleep(0.01)

    def start(self):
        send_thread = threading.Thread(target=self.send_loop, daemon=True)
        recv_thread = threading.Thread(target=self.recv_loop, daemon=True)

        send_thread.start()
        recv_thread.start()

        print("=" * 60)
        print("RTP Audio Client")
        print("=" * 60)
        print(f"Local port:  {self.local_port}")
        print(f"Remote:      {self.remote_ip}:{self.remote_port}")
        print(f"Audio:       L16 PCM @ {SAMPLE_RATE}Hz mono")
        print("=" * 60)
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
        self.stream.stop_stream()
        self.stream.close()
        self.audio.terminate()
        self.send_sock.close()
        self.recv_sock.close()


def main():
    parser = argparse.ArgumentParser(description="RTP audio client (L16 @ 24kHz)")
    parser.add_argument("--local-port", type=int, default=DEFAULT_LOCAL_PORT)
    parser.add_argument("--remote-ip", default=DEFAULT_REMOTE_IP)
    parser.add_argument("--remote-port", type=int, default=DEFAULT_REMOTE_PORT)
    args = parser.parse_args()

    client = RTPClient(args.local_port, args.remote_ip, args.remote_port)

    try:
        client.start()
    finally:
        client.cleanup()


if __name__ == "__main__":
    main()
