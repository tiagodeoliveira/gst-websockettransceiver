#!/usr/bin/env python3
"""Simple WebSocket echo server for testing."""

import asyncio
import signal
from websockets.asyncio.server import serve

PORT = 9999

async def echo(websocket):
    """Echo all received binary messages back."""
    try:
        async for message in websocket:
            if isinstance(message, bytes):
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
