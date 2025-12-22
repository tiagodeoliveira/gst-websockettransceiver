#!/usr/bin/env python3
"""Simple WebSocket echo server for testing."""

import asyncio
import logging
import signal

# Support both old and new websockets API
try:
    from websockets.asyncio.server import serve
except ImportError:
    from websockets import serve

PORT = 9999

# Suppress noisy connection errors from health checks
logging.getLogger("websockets").setLevel(logging.CRITICAL)

async def echo(websocket, path=None):
    """Echo binary messages, handle text commands."""
    binary_count = 0
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                # Echo binary messages back
                await websocket.send(message)
                binary_count += 1
                # After 3rd binary message, send a clear command to test barge-in
                if binary_count == 3:
                    await websocket.send('{"type": "clear"}')
            elif isinstance(message, str):
                # Handle text commands
                if message == "send_clear":
                    # Send clear control message back
                    await websocket.send('{"type": "clear"}')
                else:
                    # Echo unknown text messages back
                    await websocket.send(message)
    except Exception:
        pass

async def main():
    stop = asyncio.get_event_loop().create_future()

    # Handle SIGTERM/SIGINT gracefully
    def handle_signal():
        stop.set_result(None)

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, handle_signal)

    async with serve(echo, "127.0.0.1", PORT):
        print(f"READY:{PORT}", flush=True)
        await stop

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
