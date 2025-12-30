# End-to-End Test

Real-time voice AI test using the GStreamer WebSocket transceiver plugin.

Supports multiple AI backends:
- **OpenAI Realtime API** (`--provider openai`)
- **Amazon Nova Sonic** (`--provider nova`)

## Architecture

```
┌─────────────┐     RTP L16     ┌─────────────────┐   WebSocket   ┌──────────────┐
│ RTP Client  │ ←─────────────→ │ GStreamer       │ ←───────────→ │ WebSocket    │ ←→ AI
│ (mic/spkr)  │    @ 24kHz      │ Bridge          │    PCM binary │ Server       │
└─────────────┘                 └─────────────────┘               └──────────────┘
```

**Audio format**: PCM 16-bit @ 24kHz mono (all components)

## Setup

1. Build the plugin:
   ```bash
   meson setup build
   meson compile -C build
   export GST_PLUGIN_PATH=$PWD/build/src
   ```

2. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

3. Configure credentials:
   ```bash
   # For OpenAI
   export OPENAI_API_KEY="sk-..."

   # For Nova Sonic
   export AWS_ACCESS_KEY_ID="..."
   export AWS_SECRET_ACCESS_KEY="..."
   export AWS_DEFAULT_REGION="us-east-1"
   ```

## Running

Start each component in a separate terminal:

### Terminal 1: WebSocket Server
```bash
# OpenAI (default)
python3 websocket_server.py --provider openai

# Or Nova Sonic
python3 websocket_server.py --provider nova
```

### Terminal 2: GStreamer Bridge
```bash
python3 rtp_websocket_bridge.py
```

### Terminal 3: RTP Client
```bash
python3 rtp_client.py
```

Speak into your microphone - the AI will respond through your speakers.

## Options

### WebSocket Server
```
--provider {openai,nova}   AI provider (default: openai)
--host HOST                Bind address (default: 127.0.0.1)
--port PORT                Listen port (default: 8765)
--system-prompt TEXT       System prompt for AI
--debug                    Enable debug logging

# OpenAI specific
--openai-api-key KEY       API key (or OPENAI_API_KEY env)
--openai-model MODEL       Model ID (default: gpt-realtime)

# Nova specific
--nova-model-id ID         Model ID (default: amazon.nova-2-sonic-v1:0)
--nova-region REGION       AWS region (default: us-east-1)
```

### RTP Bridge
```
--rtp-port PORT            Listen for client RTP (default: 5060)
--client-ip IP             Client IP (default: 127.0.0.1)
--client-rtp-port PORT     Send RTP to client (default: 10000)
--ws-uri URI               WebSocket URI (default: ws://localhost:8765)
--gst-debug LEVEL          GStreamer debug (e.g., websockettransceiver:5)
```

### RTP Client
```
--local-port PORT          Receive RTP (default: 10000)
--remote-ip IP             Bridge IP (default: 127.0.0.1)
--remote-port PORT         Bridge RTP port (default: 5060)
```

## Features

- **Barge-in**: Interrupt AI by speaking (clears audio queue)
- **Low latency**: 20ms frame duration
- **Bidirectional**: Full-duplex audio streaming
