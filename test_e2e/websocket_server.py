#!/usr/bin/env python3
"""
WebSocket bridge between GStreamer and AI voice services.
Supports OpenAI Realtime API and Amazon Nova Sonic.

Audio format: PCM 16-bit @ 24kHz mono (both input and output)
"""

import abc
import asyncio
import base64
import json
import logging
import os
import sys
import uuid
from typing import Optional

import websockets
from websockets.server import WebSocketServerProtocol

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765

# Common audio format for all providers
SAMPLE_RATE = 24000
CHANNELS = 1
BITS_PER_SAMPLE = 16


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


class AIProvider(abc.ABC):
    """Base class for AI voice providers."""

    def __init__(self, audio_callback, barge_in_callback, call_id: str):
        self.audio_callback = audio_callback
        self.barge_in_callback = barge_in_callback
        self.call_id = call_id
        self.logger = logging.getLogger(f"{self.__class__.__name__}-{call_id}")

    @abc.abstractmethod
    async def connect(self):
        """Establish connection to the AI service."""
        pass

    @abc.abstractmethod
    async def send_audio(self, audio_bytes: bytes):
        """Send audio to the AI service."""
        pass

    @abc.abstractmethod
    async def close(self):
        """Close connection to the AI service."""
        pass


class OpenAIProvider(AIProvider):
    """OpenAI Realtime API provider."""

    def __init__(
        self,
        api_key: str,
        model: str,
        system_prompt: str,
        audio_callback,
        barge_in_callback,
        call_id: str,
    ):
        super().__init__(audio_callback, barge_in_callback, call_id)
        self.api_key = api_key
        self.model = model
        self.system_prompt = system_prompt
        self.ws: Optional[WebSocketServerProtocol] = None
        self.connected = False

    async def connect(self):
        """Establish WebSocket connection to OpenAI."""
        url = f"wss://api.openai.com/v1/realtime?model={self.model}"
        headers = {"Authorization": f"Bearer {self.api_key}"}

        try:
            self.ws = await websockets.connect(url, additional_headers=headers)
            self.connected = True
            self.logger.info(f"Connected to OpenAI: {self.model}")

            await self._configure_session()
            asyncio.create_task(self._receive_messages())

        except Exception as e:
            self.logger.error(f"Failed to connect to OpenAI: {e}")
            raise

    async def _configure_session(self):
        """Configure OpenAI session with PCM @ 24kHz."""
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
                        "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                        "transcription": {"model": "whisper-1"},
                        "turn_detection": {
                            "type": "server_vad",
                            "threshold": 0.5,
                            "prefix_padding_ms": 300,
                            "silence_duration_ms": 500,
                            "create_response": True,
                            "interrupt_response": True,
                        },
                    },
                    "output": {
                        "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                        "voice": "alloy",
                    },
                },
            },
        }

        await self.ws.send(json.dumps(session_config))
        self.logger.info(f"Session configured: PCM @ {SAMPLE_RATE}Hz")

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
        self.logger.debug(f"OpenAI event: {msg_type}")

        if msg_type == "error":
            self.logger.error(f"OpenAI error: {data.get('error')}")

        elif msg_type == "session.created":
            self.logger.debug("Session created")

        elif msg_type == "session.updated":
            self.logger.info("Session updated - triggering greeting")
            await self._trigger_greeting()

        elif msg_type in ("response.audio.delta", "response.output_audio.delta"):
            audio_base64 = data.get("delta")
            if audio_base64 and self.audio_callback:
                audio_bytes = base64.b64decode(audio_base64)
                self.logger.debug(f"Received {len(audio_bytes)} bytes audio from OpenAI")
                await self.audio_callback(audio_bytes)

        elif msg_type == "response.audio.done":
            self.logger.debug("Audio response complete")

        elif msg_type == "input_audio_buffer.speech_started":
            self.logger.info("User speech started - barge-in")
            if self.barge_in_callback:
                await self.barge_in_callback()

        elif msg_type == "input_audio_buffer.speech_stopped":
            self.logger.debug("User speech stopped - triggering response")
            # Manually trigger response in case turn_detection.create_response isn't working
            await self.ws.send(json.dumps({"type": "response.create"}))

        elif msg_type == "conversation.item.input_audio_transcription.completed":
            transcript = data.get("transcript", "")
            if transcript:
                self.logger.info(f"User: {transcript}")

        elif msg_type == "response.audio_transcript.done":
            transcript = data.get("transcript", "")
            if transcript:
                self.logger.info(f"Assistant: {transcript}")

    async def _trigger_greeting(self):
        """Trigger OpenAI to generate a greeting."""
        if not self.connected or not self.ws:
            return

        try:
            greeting_item = {
                "type": "conversation.item.create",
                "item": {
                    "type": "message",
                    "role": "user",
                    "content": [
                        {
                            "type": "input_text",
                            "text": "Greet the user briefly and ask how you can help.",
                        }
                    ],
                },
            }
            await self.ws.send(json.dumps(greeting_item))
            await self.ws.send(json.dumps({"type": "response.create"}))
            self.logger.info("Greeting triggered")
        except Exception as e:
            self.logger.error(f"Error triggering greeting: {e}")

    async def send_audio(self, audio_bytes: bytes):
        """Send audio to OpenAI."""
        if not self.connected or not self.ws:
            return

        try:
            audio_base64 = base64.b64encode(audio_bytes).decode("utf-8")
            message = {"type": "input_audio_buffer.append", "audio": audio_base64}
            await self.ws.send(json.dumps(message))
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


class NovaProvider(AIProvider):
    """Amazon Nova Sonic provider via Bedrock SDK."""

    def __init__(
        self,
        model_id: str,
        region: str,
        system_prompt: str,
        audio_callback,
        barge_in_callback,
        call_id: str,
    ):
        super().__init__(audio_callback, barge_in_callback, call_id)
        self.model_id = model_id
        self.region = region
        self.system_prompt = system_prompt

        self.client = None
        self.stream = None
        self.is_active = False
        self.prompt_name = str(uuid.uuid4())
        self.content_name = str(uuid.uuid4())
        self.audio_content_name = str(uuid.uuid4())

    def _initialize_client(self):
        """Initialize the Bedrock client."""
        from aws_sdk_bedrock_runtime.client import BedrockRuntimeClient
        from aws_sdk_bedrock_runtime.config import Config
        from smithy_aws_core.identity.environment import EnvironmentCredentialsResolver

        config = Config(
            endpoint_uri=f"https://bedrock-runtime.{self.region}.amazonaws.com",
            region=self.region,
            aws_credentials_identity_resolver=EnvironmentCredentialsResolver(),
        )
        self.client = BedrockRuntimeClient(config=config)
        self.logger.info(f"Bedrock client initialized: {self.region}")

    async def _send_event(self, event_json: str):
        """Send an event to the stream."""
        if not self.stream:
            return

        from aws_sdk_bedrock_runtime.models import (
            InvokeModelWithBidirectionalStreamInputChunk,
            BidirectionalInputPayloadPart,
        )

        event = InvokeModelWithBidirectionalStreamInputChunk(
            value=BidirectionalInputPayloadPart(bytes_=event_json.encode("utf-8"))
        )
        await self.stream.input_stream.send(event)

    async def connect(self):
        """Establish connection to Nova Sonic."""
        from aws_sdk_bedrock_runtime.client import (
            InvokeModelWithBidirectionalStreamOperationInput,
        )

        try:
            if not self.client:
                self._initialize_client()

            self.stream = await self.client.invoke_model_with_bidirectional_stream(
                InvokeModelWithBidirectionalStreamOperationInput(model_id=self.model_id)
            )
            self.is_active = True
            self.logger.info(f"Connected to Nova Sonic: {self.model_id}")

            # Session setup
            await self._send_event(json.dumps({
                "event": {
                    "sessionStart": {
                        "inferenceConfiguration": {
                            "maxTokens": 1024,
                            "topP": 0.9,
                            "temperature": 0.7,
                        }
                    }
                }
            }))

            # Prompt start with audio output config
            await self._send_event(json.dumps({
                "event": {
                    "promptStart": {
                        "promptName": self.prompt_name,
                        "textOutputConfiguration": {"mediaType": "text/plain"},
                        "audioOutputConfiguration": {
                            "mediaType": "audio/lpcm",
                            "sampleRateHertz": SAMPLE_RATE,
                            "sampleSizeBits": BITS_PER_SAMPLE,
                            "channelCount": CHANNELS,
                            "voiceId": "matthew",
                            "encoding": "base64",
                            "audioType": "SPEECH",
                        }
                    }
                }
            }))

            # System prompt
            await self._send_event(json.dumps({
                "event": {
                    "contentStart": {
                        "promptName": self.prompt_name,
                        "contentName": self.content_name,
                        "type": "TEXT",
                        "interactive": False,
                        "role": "SYSTEM",
                        "textInputConfiguration": {"mediaType": "text/plain"}
                    }
                }
            }))

            await self._send_event(json.dumps({
                "event": {
                    "textInput": {
                        "promptName": self.prompt_name,
                        "contentName": self.content_name,
                        "content": self.system_prompt,
                    }
                }
            }))

            await self._send_event(json.dumps({
                "event": {
                    "contentEnd": {
                        "promptName": self.prompt_name,
                        "contentName": self.content_name,
                    }
                }
            }))

            # Start audio input
            await self._send_event(json.dumps({
                "event": {
                    "contentStart": {
                        "promptName": self.prompt_name,
                        "contentName": self.audio_content_name,
                        "type": "AUDIO",
                        "interactive": True,
                        "role": "USER",
                        "audioInputConfiguration": {
                            "mediaType": "audio/lpcm",
                            "sampleRateHertz": SAMPLE_RATE,
                            "sampleSizeBits": BITS_PER_SAMPLE,
                            "channelCount": CHANNELS,
                            "audioType": "SPEECH",
                            "encoding": "base64",
                        }
                    }
                }
            }))

            self.logger.info("Session configured: PCM16 @ 24kHz")
            asyncio.create_task(self._process_responses())

            # Trigger greeting after session is ready
            await self._trigger_greeting()

        except Exception as e:
            self.logger.error(f"Failed to connect to Nova Sonic: {e}")
            raise

    async def _trigger_greeting(self):
        """Trigger Nova Sonic to generate a greeting."""
        if not self.is_active:
            return

        try:
            text_prompt_id = str(uuid.uuid4())

            # contentStart for text prompt
            await self._send_event(json.dumps({
                "event": {
                    "contentStart": {
                        "promptName": self.prompt_name,
                        "contentName": text_prompt_id,
                        "type": "TEXT",
                        "interactive": True,
                        "role": "USER",
                        "textInputConfiguration": {"mediaType": "text/plain"}
                    }
                }
            }))

            # textInput with greeting prompt
            await self._send_event(json.dumps({
                "event": {
                    "textInput": {
                        "promptName": self.prompt_name,
                        "contentName": text_prompt_id,
                        "content": "Greet the user briefly and ask how you can help.",
                    }
                }
            }))

            # contentEnd
            await self._send_event(json.dumps({
                "event": {
                    "contentEnd": {
                        "promptName": self.prompt_name,
                        "contentName": text_prompt_id,
                    }
                }
            }))

            self.logger.info("Greeting triggered")
        except Exception as e:
            self.logger.error(f"Error triggering greeting: {e}")

    async def _process_responses(self):
        """Process responses from Nova Sonic."""
        try:
            while self.is_active:
                output = await self.stream.await_output()
                result = await output[1].receive()

                if result.value and result.value.bytes_:
                    response_data = result.value.bytes_.decode("utf-8")
                    json_data = json.loads(response_data)
                    await self._handle_response(json_data)

        except asyncio.CancelledError:
            self.logger.info("Response processing cancelled")
        except Exception as e:
            self.logger.error(f"Error processing responses: {e}")
            self.is_active = False

    async def _handle_response(self, data: dict):
        """Handle response events from Nova Sonic."""
        if "event" not in data:
            return

        event = data["event"]

        if "audioOutput" in event:
            audio_content = event["audioOutput"].get("content")
            if audio_content and self.audio_callback:
                audio_bytes = base64.b64decode(audio_content)
                await self.audio_callback(audio_bytes)

        elif "textOutput" in event:
            text = event["textOutput"].get("content", "")
            role = event.get("role", "")
            if text:
                self.logger.info(f"{role}: {text}")

        elif "contentStart" in event:
            # Could detect speech started for barge-in
            pass

    async def send_audio(self, audio_bytes: bytes):
        """Send audio to Nova Sonic."""
        if not self.is_active:
            return

        try:
            audio_base64 = base64.b64encode(audio_bytes).decode("utf-8")
            await self._send_event(json.dumps({
                "event": {
                    "audioInput": {
                        "promptName": self.prompt_name,
                        "contentName": self.audio_content_name,
                        "content": audio_base64,
                    }
                }
            }))
        except Exception as e:
            self.logger.error(f"Error sending audio: {e}")

    async def close(self):
        """Close connection to Nova Sonic."""
        self.is_active = False

        if self.stream:
            try:
                await self._send_event(json.dumps({
                    "event": {
                        "contentEnd": {
                            "promptName": self.prompt_name,
                            "contentName": self.audio_content_name,
                        }
                    }
                }))
                await self._send_event(json.dumps({
                    "event": {"promptEnd": {"promptName": self.prompt_name}}
                }))
                await self._send_event(json.dumps({"event": {"sessionEnd": {}}}))
                await self.stream.input_stream.close()
            except Exception as e:
                self.logger.warning(f"Error during cleanup: {e}")

        self.logger.info("Nova Sonic connection closed")


class ClientHandler:
    """Handles individual GStreamer client connections."""

    def __init__(self, websocket: WebSocketServerProtocol, call_id: str, provider: AIProvider):
        self.websocket = websocket
        self.call_id = call_id
        self.provider = provider
        self.logger = logging.getLogger(f"Client-{call_id}")

    async def handle(self):
        """Handle client connection lifecycle."""
        try:
            self.logger.info("Client connected")
            await self.provider.connect()

            async for message in self.websocket:
                if isinstance(message, bytes):
                    await self.provider.send_audio(message)
                else:
                    data = json.loads(message)
                    if data.get("type") == "audio":
                        audio_bytes = base64.b64decode(data.get("data", ""))
                        await self.provider.send_audio(audio_bytes)

        except websockets.exceptions.ConnectionClosed:
            self.logger.info("Client disconnected")
        except Exception as e:
            self.logger.error(f"Error handling client: {e}", exc_info=True)
        finally:
            await self.provider.close()

    async def send_audio_to_client(self, audio_bytes: bytes):
        """Send audio from AI to GStreamer client."""
        try:
            self.logger.debug(f"Sending {len(audio_bytes)} bytes to GStreamer client")
            await self.websocket.send(audio_bytes)
        except Exception as e:
            self.logger.error(f"Error sending audio to client: {e}")

    async def send_barge_in(self):
        """Send barge-in clear command to GStreamer client."""
        try:
            self.logger.info("Sending barge-in clear command")
            await self.websocket.send('{"type": "clear"}')
        except Exception as e:
            self.logger.error(f"Error sending barge-in: {e}")


class WebSocketServer:
    """WebSocket server for handling GStreamer clients."""

    def __init__(self, host: str, port: int, provider_type: str, provider_config: dict):
        self.host = host
        self.port = port
        self.provider_type = provider_type
        self.provider_config = provider_config
        self.logger = logging.getLogger("WebSocketServer")
        self.client_counter = 0

    def _create_provider(self, audio_callback, barge_in_callback, call_id: str) -> AIProvider:
        """Create an AI provider instance."""
        if self.provider_type == "openai":
            return OpenAIProvider(
                api_key=self.provider_config["api_key"],
                model=self.provider_config["model"],
                system_prompt=self.provider_config["system_prompt"],
                audio_callback=audio_callback,
                barge_in_callback=barge_in_callback,
                call_id=call_id,
            )
        elif self.provider_type == "nova":
            return NovaProvider(
                model_id=self.provider_config["model_id"],
                region=self.provider_config["region"],
                system_prompt=self.provider_config["system_prompt"],
                audio_callback=audio_callback,
                barge_in_callback=barge_in_callback,
                call_id=call_id,
            )
        else:
            raise ValueError(f"Unknown provider: {self.provider_type}")

    async def handle_client(self, websocket: WebSocketServerProtocol):
        """Handle new client connection."""
        self.client_counter += 1
        call_id = f"call-{self.client_counter}"

        handler = ClientHandler(websocket, call_id, provider=None)
        provider = self._create_provider(
            audio_callback=handler.send_audio_to_client,
            barge_in_callback=handler.send_barge_in,
            call_id=call_id,
        )
        handler.provider = provider

        await handler.handle()

    async def start(self):
        """Start the WebSocket server."""
        self.logger.info(f"Starting server on {self.host}:{self.port}")
        self.logger.info(f"Provider: {self.provider_type}")
        self.logger.info(f"Audio: PCM16 @ {SAMPLE_RATE}Hz mono")

        async with websockets.serve(self.handle_client, self.host, self.port):
            self.logger.info("Server ready, waiting for connections...")
            await asyncio.Future()


async def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="WebSocket server bridging GStreamer to AI voice services"
    )
    parser.add_argument(
        "--provider",
        choices=["openai", "nova"],
        default="openai",
        help="AI provider to use (default: openai)",
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument(
        "--system-prompt",
        default="You are a helpful voice assistant. Be concise.",
    )

    # OpenAI options
    parser.add_argument("--openai-api-key", default=os.environ.get("OPENAI_API_KEY"))
    parser.add_argument(
        "--openai-model",
        default=os.environ.get("OPENAI_MODEL", "gpt-realtime"),
    )

    # Nova options
    parser.add_argument(
        "--nova-model-id",
        default=os.environ.get("NOVA_MODEL_ID", "amazon.nova-2-sonic-v1:0"),
    )
    parser.add_argument(
        "--nova-region",
        default=os.environ.get("AWS_DEFAULT_REGION", "us-east-1"),
    )

    args = parser.parse_args()

    global logger
    logger = setup_logging(args.debug)

    # Build provider config
    if args.provider == "openai":
        if not args.openai_api_key:
            logger.error("OPENAI_API_KEY not set")
            sys.exit(1)
        provider_config = {
            "api_key": args.openai_api_key,
            "model": args.openai_model,
            "system_prompt": args.system_prompt,
        }
    else:  # nova
        provider_config = {
            "model_id": args.nova_model_id,
            "region": args.nova_region,
            "system_prompt": args.system_prompt,
        }

    try:
        server = WebSocketServer(
            host=args.host,
            port=args.port,
            provider_type=args.provider,
            provider_config=provider_config,
        )
        await server.start()
    except KeyboardInterrupt:
        logger.info("Shutting down...")
    except Exception as e:
        logger.error(f"Server error: {e}", exc_info=True)
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
