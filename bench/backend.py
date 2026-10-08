#!/usr/bin/env python3
"""Bounded-memory asyncio HTTP fixture; always closes to match proxy semantics."""
import argparse
import asyncio
import ssl

BLOCK=bytes(range(256))*256

async def serve(reader,writer):
    try:
        headers=await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'),15)
        lines=headers.decode('ascii').split('\r\n')
        length=next((int(line.split(':',1)[1]) for line in lines[1:] if line.lower().startswith('content-length:')),0)
        while length:
            data=await reader.read(min(length,65536))
            if not data:
                return
            length-=len(data)
        path=lines[0].split()[1]
        size=int(path.split('bytes=',1)[1]) if 'bytes=' in path else 16
        writer.write(f'HTTP/1.1 200 OK\r\nContent-Length: {size}\r\nConnection: close\r\n\r\n'.encode())
        while size:
            n=min(size,len(BLOCK))
            writer.write(BLOCK[:n])
            await writer.drain()
            size-=n
    except (OSError,asyncio.IncompleteReadError,asyncio.LimitOverrunError,asyncio.TimeoutError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except OSError:
            pass

async def main():
    p=argparse.ArgumentParser()
    p.add_argument('--port',type=int,required=True)
    p.add_argument('--cert')
    p.add_argument('--key')
    a=p.parse_args()
    ctx=None
    if a.cert:
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a.cert,a.key)
    server=await asyncio.start_server(serve,'127.0.0.1',a.port,ssl=ctx,backlog=4096)
    async with server:
        await server.serve_forever()

if __name__=='__main__':
    asyncio.run(main())
