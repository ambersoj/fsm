1. Identity / lifecycle
1.1 NET.init.done
1.2 NET.ready
1.3 NET.error
{ "message": "...", "code": optional }

2. Link state
2.1 NET.link.up
2.2 NET.link.down

3. Receive path
3.1 NET.rx.progress
{
  "chunk": <uint32>,
  "offset": <uint64>,
  "eof": <bool>
}
3.2 NET.rx.done
{
  "chunks": <uint32>,
  "bytes": <uint64>
}

4. Transmit path
4.1 NET.tx.progress
{
  "chunk": <uint32>,
  "offset": <uint64>,
  "eof": <bool>
}
4.2 NET.tx.done
{
  "chunks": <uint32>,
  "bytes": <uint64>
}

5. Chunk-level semantics
5.1 NET.chunk.progress
{
  "index": <uint32>,
  "size": <uint32>
}
5.2 NET.chunk.done
