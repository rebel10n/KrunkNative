"""WebSocket interop: python `websockets` client <-> knsvc server, and knsvc client <-> python server.
Usage: ws_interop.py <path to knsvc_test_ws>. Exit 77 (ctest skip) when `websockets` is missing."""
import asyncio
import subprocess
import sys

try:
    import websockets
    from websockets.asyncio.client import connect
    from websockets.asyncio.server import serve
except ImportError:
    print("SKIP: python websockets module not installed")
    sys.exit(77)

BIN = sys.argv[1]
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


async def client_against_knsvc(port):
    url = f"ws://127.0.0.1:{port}/echo"
    async with connect(url, subprotocols=["kn1"], max_size=None, compression=None) as ws:
        check(ws.subprotocol == "kn1", "subprotocol negotiated")
        await ws.send("héllo ✓")
        check(await ws.recv() == "héllo ✓", "text echo")
        data = bytes(range(256)) * 200
        await ws.send(data)
        check(await ws.recv() == data, "binary echo 51200 bytes")
        await ws.send(["frag", "men", "ted"])  # fragmented text message
        check(await ws.recv() == "fragmented", "fragmented echo")
        await ws.send([b"\x00\x01", b"\x02"])
        check(await ws.recv() == b"\x00\x01\x02", "fragmented binary echo")
        pong = await ws.ping(b"abc")
        await asyncio.wait_for(pong, 2)
        for i in range(200):  # many small messages
            await ws.send(f"m{i}")
        got = [await ws.recv() for _ in range(200)]
        check(got == [f"m{i}" for i in range(200)], "200 message ordering")
        await ws.close(code=4002, reason="py bye")
        check(ws.close_code == 4002, f"close code echoed ({ws.close_code})")
    # too large (server limit 64 KiB) -> 1009
    async with connect(url, max_size=None, compression=None) as ws:
        await ws.send(b"x" * 70000)
        try:
            await asyncio.wait_for(ws.recv(), 2)
            check(False, "oversized message rejected")
        except websockets.exceptions.ConnectionClosed as e:
            check(e.rcvd is not None and e.rcvd.code == 1009, f"oversized -> 1009 ({e.rcvd})")
    # permessage-deflate offered by the client is declined
    async with connect(url, compression="deflate") as ws:
        check(not ws.response.headers.get("Sec-WebSocket-Extensions"), "deflate declined")
        await ws.send("plain")
        check(await ws.recv() == "plain", "echo without extensions")


async def python_server_for_knsvc_client():
    async def echo(ws):
        async for msg in ws:
            await ws.send(msg)

    async with serve(echo, "127.0.0.1", 0, max_size=None, compression=None) as server:
        port = server.sockets[0].getsockname()[1]
        proc = await asyncio.create_subprocess_exec(BIN, "--client", f"ws://127.0.0.1:{port}/x",
                                                    stdout=subprocess.PIPE)
        out, _ = await asyncio.wait_for(proc.communicate(), 10)
        check(proc.returncode == 0, f"knsvc client vs python server: {out.decode().strip()}")


async def main():
    proc = subprocess.Popen([BIN, "--serve"], stdout=subprocess.PIPE, text=True)
    try:
        line = proc.stdout.readline().split()
        port = int(line[1])
        await client_against_knsvc(port)
    finally:
        proc.terminate()
        proc.wait(5)
    await python_server_for_knsvc_client()


asyncio.run(main())
print(f"ws_interop: websockets {websockets.__version__}, {len(failures)} failures")
sys.exit(1 if failures else 0)
