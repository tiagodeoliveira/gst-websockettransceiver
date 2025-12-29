# GStreamer WebSocket Transceiver Plugin

GStreamer element that sends and receives audio over WebSocket connections. Bidirectional audio streaming for AI voice bots and real-time communication.

## Requirements

- GStreamer 1.0+
- libsoup-3.0
- Meson build system

## Build

```bash
meson setup build
meson compile -C build
```

## Install

Add to GStreamer plugin path:

```bash
export GST_PLUGIN_PATH="/path/to/gst-websockettransceiver/build/src"
```

Verify installation:

```bash
gst-inspect-1.0 websockettransceiver
```

## Usage

```bash
gst-launch-1.0 \
  audiotestsrc ! \
  audio/x-raw,format=S16LE,rate=16000,channels=1 ! \
  websockettransceiver uri=ws://localhost:8765 ! \
  audioconvert ! \
  autoaudiosink
```

## Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `uri` | string | NULL | WebSocket URI to connect to (required) |
| `sample-rate` | uint | 16000 | Audio sample rate in Hz |
| `channels` | uint | 1 | Number of audio channels (1 or 2) |
| `frame-duration-ms` | uint | 250 | Frame duration in milliseconds |
| `max-queue-size` | uint | 100 | Maximum receive queue size in buffers |
| `initial-buffer-count` | uint | 3 | Buffers to accumulate before playback (0 = no buffering) |
| `reconnect-enabled` | boolean | true | Enable automatic reconnection on disconnect |
| `initial-reconnect-delay-ms` | uint | 1000 | Initial backoff delay (ms) |
| `max-backoff-ms` | uint | 30000 | Maximum backoff delay (ms) |
| `max-reconnects` | uint | 10 | Maximum reconnection attempts (0 = unlimited) |

## Supported Formats

- **audio/x-raw**: S16LE, S16BE, S32LE, S32BE, F32LE, F32BE
- **audio/x-mulaw**: G.711 μ-law
- **audio/x-alaw**: G.711 A-law

Sample rates: 8000-48000 Hz
Channels: 1-2 (mono/stereo)

## Control Messages

The element supports JSON control messages via WebSocket text frames for pipeline control.

### Clear/Barge-in

To clear all pending audio buffers (useful for voice AI barge-in scenarios):

```json
{"type": "clear"}
```

When received, this command:
- Clears all buffered audio in the receive queue
- Resets timestamps for seamless continuation
- Sends flush events downstream to clear the pipeline

This enables immediate interruption of AI speech when the user starts talking.

## Examples

### Send microphone to WebSocket, receive from WebSocket to speaker

```bash
gst-launch-1.0 \
  autoaudiosrc ! \
  audioconvert ! \
  audio/x-mulaw,rate=8000,channels=1 ! \
  websockettransceiver uri=ws://localhost:8765 ! \
  audio/x-mulaw,rate=8000,channels=1 ! \
  mulawdec ! \
  audioconvert ! \
  autoaudiosink
```

### RTP to WebSocket bridge

```bash
gst-launch-1.0 \
  udpsrc port=5060 caps="application/x-rtp,media=audio,clock-rate=8000,encoding-name=PCMU" ! \
  rtpjitterbuffer ! \
  rtppcmudepay ! \
  websockettransceiver uri=ws://localhost:8765 ! \
  rtppcmupay ! \
  udpsink host=127.0.0.1 port=5000
```

## Testing

### Run all tests

```bash
meson test -C build -v
```

### Test structure

- **Unit tests** (`tests/check/elements/websockettransceiver.c`): Fast tests for element creation, properties, pads, and state changes. No network required.
- **Integration tests** (`tests/check/elements/websockettransceiver_integration.c`): Tests with a real WebSocket server. Verifies connection, data sending, and multiple buffer handling.

### Run specific test suite

```bash
# Unit tests only
./build/tests/test_websockettransceiver

# Integration tests only (starts stub WebSocket server automatically)
./tests/run_integration_test.sh ./build/tests/test_websockettransceiver_integration
```

### Static analysis

```bash
meson compile -C build cppcheck
```

## Docker

Build and test without installing GStreamer locally:

```bash
# Build the image
docker build -t gst-websockettransceiver .

# Run all tests
docker run --rm gst-websockettransceiver

# Interactive shell for debugging
docker run --rm -it gst-websockettransceiver bash

# Run specific commands
docker run --rm gst-websockettransceiver meson test -C build --list
```

## Architecture

The plugin is a bidirectional element with both sink and source pads:

- **Sink pad**: Receives audio from upstream, sends over WebSocket
- **Source pad**: Receives audio from WebSocket, pushes downstream

Runs two threads:
- WebSocket thread: Handles connection and message I/O
- Output thread: Paced buffer delivery at configured frame rate

