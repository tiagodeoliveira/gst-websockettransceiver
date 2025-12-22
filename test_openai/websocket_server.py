#!/usr/bin/env python3
"""
WebSocket bridge between GStreamer and OpenAI Realtime API.
Forwards G.711 μ-law audio bidirectionally.
"""

import asyncio
import base64
import json
import logging
import os
import sys
from typing import Optional

import websockets
from websockets.server import WebSocketServerProtocol

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765


def setup_logging(debug: bool = False) -> logging.Logger:
    level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s - %(levelname)s - [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        force=True,
    )
    logging.getLogger("websockets.client").setLevel(logging.INFO)
    logging.getLogger("websockets.server").setLevel(logging.INFO)
    return logging.getLogger(__name__)


logger = setup_logging()


class OpenAIRealtimeClient:
    """Manages connection to OpenAI Realtime API."""

    def __init__(
        self, api_key: str, model: str, system_prompt: str, audio_callback, barge_in_callback, call_id: str
    ):
        self.api_key = api_key
        self.model = model
        self.system_prompt = system_prompt
        self.audio_callback = audio_callback
        self.barge_in_callback = barge_in_callback
        self.call_id = call_id

        self.ws: Optional[WebSocketServerProtocol] = None
        self.connected = False
        self.logger = logging.getLogger(f"OpenAIClient-{call_id}")

    async def connect(self):
        """Establish WebSocket connection to OpenAI."""
        url = f"wss://api.openai.com/v1/realtime?model={self.model}"
        headers = {"Authorization": f"Bearer {self.api_key}"}

        try:
            self.ws = await websockets.connect(url, additional_headers=headers)
            self.connected = True
            self.logger.info(f"Connected to OpenAI: {url}")

            await self._configure_session()

            # Start receive task
            asyncio.create_task(self._receive_messages())

        except Exception as e:
            self.logger.error(f"Failed to connect to OpenAI: {e}")
            raise

    async def _configure_session(self):
        """Configure OpenAI session with G.711 μ-law format."""
        session_config = {
            "type": "session.update",
            "session": {
                "type": "realtime",
                "model": self.model,
                "output_modalities": ["audio"],
                "instructions": self.system_prompt,
                "audio": {
                    "input": {
                        "noise_reduction": {"type": "near_field"},
                        "format": {"type": "audio/pcmu"},
                        "transcription": {"model": "whisper-1"},
                        "turn_detection": {
                            "type": "semantic_vad",
                            "eagerness": "auto",
                            "create_response": True,
                            "interrupt_response": True,
                        },
                    },
                    "output": {
                        "format": {"type": "audio/pcmu"},
                    },
                },
            },
        }

        self.logger.debug(f"Configuring session: {json.dumps(session_config)}")
        await self.ws.send(json.dumps(session_config))
        self.logger.info("Session configured with G.711 μ-law @ 8kHz")

    async def _receive_messages(self):
        """Receive and process messages from OpenAI."""
        try:
            async for message in self.ws:
                data = json.loads(message)
                await self._handle_message(data)
        except websockets.exceptions.ConnectionClosed:
            self.logger.warning("OpenAI connection closed")
            self.connected = False
        except Exception as e:
            self.logger.error(f"Error receiving messages: {e}")
            self.connected = False

    async def _handle_message(self, data: dict):
        """Handle messages from OpenAI."""
        msg_type = data.get("type")

        if msg_type == "error":
            self.logger.error(f"OpenAI error: {data.get('error')}")

        elif msg_type == "session.created":
            self.logger.debug("Session created")

        elif msg_type == "session.updated":
            self.logger.info("Session updated - triggering greeting")
            # Trigger greeting response after session is ready
            await self.trigger_greeting()

        elif msg_type in {"response.audio.delta", "response.output_audio.delta"}:
            audio_base64 = data.get("delta")
            if audio_base64 and self.audio_callback:
                audio_bytes = base64.b64decode(audio_base64)
                await self.audio_callback(audio_bytes)

        elif msg_type in {"response.audio.done", "response.output_audio.done"}:
            self.logger.info("Audio response complete")

        elif msg_type == "response.created":
            response_id = data.get("response", {}).get("id", "unknown")
            self.logger.info(f"Response created (id={response_id})")

        elif msg_type == "input_audio_buffer.speech_started":
            self.logger.info("User speech started - triggering barge-in")
            if self.barge_in_callback:
                await self.barge_in_callback()

        elif msg_type == "input_audio_buffer.speech_stopped":
            self.logger.info("User speech stopped")

        elif msg_type == "conversation.item.input_audio_transcription.completed":
            transcript = data.get("transcript", "")
            if transcript:
                self.logger.info(f"Input: {transcript}")

        elif msg_type in {
            "response.audio_transcript.done",
            "response.output_audio_transcript.done",
        }:
            transcript = data.get("transcript", "")
            if transcript:
                self.logger.info(f"Output: {transcript}")

        else:
            self.logger.info(f"Unhandled message type: {msg_type}")

    async def trigger_greeting(self):
        """Trigger OpenAI to generate a greeting response with specific instructions."""
        if not self.connected or not self.ws:
            self.logger.warning("Not connected, cannot trigger greeting")
            return

        try:
            # Add a conversation item with greeting instructions
            initial_conversation_item = {
                "type": "conversation.item.create",
                "item": {
                    "type": "message",
                    "role": "user",
                    "content": [
                        {
                            "type": "input_text",
                            "text": "Greet the user with 'Hello there! I am an AI voice assistant. You can ask me for facts, jokes, or anything you can imagine. How can I help you?'",
                        }
                    ],
                },
            }
            await self.ws.send(json.dumps(initial_conversation_item))
            self.logger.debug("Sent greeting conversation item")

            # Trigger response creation
            response_create = {"type": "response.create"}
            await self.ws.send(json.dumps(response_create))
            self.logger.info("Greeting triggered")

        except Exception as e:
            self.logger.error(f"Error triggering greeting: {e}")

    async def send_audio(self, audio_bytes: bytes):
        """Send audio to OpenAI."""
        if not self.connected or not self.ws:
            self.logger.warning("Not connected, dropping audio")
            return

        try:
            audio_base64 = base64.b64encode(audio_bytes).decode("utf-8")
            message = {"type": "input_audio_buffer.append", "audio": audio_base64}
            await self.ws.send(json.dumps(message))
            self.logger.debug(f"Sent {len(audio_bytes)} bytes to OpenAI")
        except Exception as e:
            self.logger.error(f"Error sending audio: {e}")

    async def close(self):
        """Close connection to OpenAI."""
        self.connected = False
        if self.ws:
            try:
                await self.ws.close()
            except:
                pass
        self.logger.info("OpenAI connection closed")


class ClientHandler:
    """Handles individual client connections."""

    def __init__(
        self,
        websocket: WebSocketServerProtocol,
        call_id: str,
        openai_api_key: str,
        openai_model: str,
        system_prompt: str,
    ):
        self.websocket = websocket
        self.call_id = call_id
        self.logger = logging.getLogger(f"Client-{call_id}")

        self.openai_client = OpenAIRealtimeClient(
            api_key=openai_api_key,
            model=openai_model,
            system_prompt=system_prompt,
            audio_callback=self._handle_openai_audio,
            barge_in_callback=self._handle_barge_in,
            call_id=call_id,
        )

    async def handle(self):
        """Handle client connection lifecycle."""
        try:
            self.logger.info("Client connected")
            await self.openai_client.connect()

            async for message in self.websocket:
                await self._handle_client_message(message)

        except websockets.exceptions.ConnectionClosed:
            self.logger.info("Client disconnected")
        except Exception as e:
            self.logger.error(f"Error handling client: {e}", exc_info=True)
        finally:
            await self.cleanup()

    async def _handle_client_message(self, message):
        """Handle message from GStreamer client."""
        try:
            if isinstance(message, bytes):
                ulaw_data = message
                await self.openai_client.send_audio(ulaw_data)
                self.logger.debug(f"Sent {len(ulaw_data)} bytes to OpenAI (binary)")
            else:
                data = json.loads(message)
                msg_type = data.get("type")

                if msg_type == "audio":
                    audio_base64 = data.get("data")
                    if not audio_base64:
                        return

                    ulaw_data = base64.b64decode(audio_base64)
                    await self.openai_client.send_audio(ulaw_data)
                    self.logger.debug(f"Sent {len(ulaw_data)} bytes to OpenAI (JSON)")
                else:
                    self.logger.debug(f"Unhandled message type: {msg_type}")

        except Exception as e:
            self.logger.error(f"Error handling client message: {e}")

    async def _handle_openai_audio(self, audio_bytes: bytes):
        """Handle audio from OpenAI and send to client."""
        try:
            self.logger.debug(
                f"Received {len(audio_bytes)} bytes from OpenAI, sending to client"
            )
            await self.websocket.send(audio_bytes)
        except Exception as e:
            self.logger.error(f"Error sending audio to client: {e}")

    async def _handle_barge_in(self):
        """Handle barge-in by sending clear command to GStreamer client."""
        try:
            self.logger.info("Sending barge-in clear command to GStreamer")
            await self.websocket.send('{"type": "clear"}')
        except Exception as e:
            self.logger.error(f"Error sending barge-in command: {e}")

    async def cleanup(self):
        """Cleanup resources."""
        self.logger.info("Cleaning up")
        await self.openai_client.close()


class WebSocketServer:
    """WebSocket server for handling GStreamer clients."""

    def __init__(self, host, port, system_prompt):
        self.host = host
        self.port = port
        self.openai_api_key = os.environ.get("OPENAI_API_KEY")
        self.openai_model = os.environ.get("OPENAI_MODEL")
        self.system_prompt = system_prompt

        self.logger = logging.getLogger("WebSocketServer")
        self.client_counter = 0

        if not self.openai_api_key:
            raise ValueError("OPENAI_API_KEY not set")

    async def handle_client(self, websocket: WebSocketServerProtocol):
        """Handle new client connection."""
        self.client_counter += 1
        call_id = f"client-{self.client_counter}"

        handler = ClientHandler(
            websocket=websocket,
            call_id=call_id,
            openai_api_key=self.openai_api_key,
            openai_model=self.openai_model,
            system_prompt=self.system_prompt,
        )

        await handler.handle()

    async def start(self):
        """Start the WebSocket server."""
        self.logger.info(f"Starting WebSocket server on {self.host}:{self.port}")

        async with websockets.serve(self.handle_client, self.host, self.port):
            self.logger.info("Server ready, waiting for connections...")
            await asyncio.Future()  # Run forever


async def main():
    """Main entry point."""
    import argparse

    parser = argparse.ArgumentParser(
        description="WebSocket server for GStreamer to OpenAI bridge"
    )
    parser.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help=f"Host to bind to (default: {DEFAULT_HOST})",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"Port to listen on (default: {DEFAULT_PORT})",
    )
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")

    parser.add_argument(
        "--system-prompt",
        default="You are a helpful assistant.",
        help="System prompt for OpenAI",
    )

    args = parser.parse_args()

    # Setup logging
    global logger
    logger = setup_logging(args.debug)

    # Create and start server
    try:
        server = WebSocketServer(
            host=args.host, port=args.port, system_prompt=args.system_prompt
        )

        await server.start()

    except KeyboardInterrupt:
        logger.info("Shutting down...")
    except Exception as e:
        logger.error(f"Server error: {e}", exc_info=True)
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
