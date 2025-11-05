# OpenAI Realtime API Test

Simple test setup for the websockettransceiver plugin with OpenAI's Realtime API.

## Requirements

- Python 3.x
- GStreamer with websockettransceiver plugin built
- OpenAI API key
- pyaudio (`pip install pyaudio`)
- websockets (`pip install websockets`)

## Setup

1. Set environment variables:
```bash
export OPENAI_API_KEY="your-api-key-here"
export OPENAI_MODEL="gpt-4o-realtime-preview-2024-12-17"
export GST_PLUGIN_PATH="/path/to/gst-websockettransceiver/build/src"
```

## Run the Test

Open 3 terminals and run in order:

**Terminal 1 - WebSocket Server (connects to OpenAI):**
```bash
python3 websocket_server.py
```

**Terminal 2 - RTP-to-WebSocket Bridge (GStreamer pipeline):**
```bash
python3 rtp_websocket_bridge.py
```

**Terminal 3 - RTP Client (microphone + speaker):**
```bash
python3 rtp_client.py
```

That's it. Talk into your microphone and the AI will respond through your speakers.

## What Each Component Does

- `websocket_server.py` - Bridges between GStreamer and OpenAI Realtime API
- `rtp_websocket_bridge.py` - GStreamer pipeline that converts RTP ↔ WebSocket
- `rtp_client.py` - Sends/receives RTP audio from your microphone/speakers
