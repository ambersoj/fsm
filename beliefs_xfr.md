1. Identity / lifecycle
1.1 XFR.init.done
1.2 XFR.ready
1.3 XFR.error
{ "message": "...", "code": optional }

2. Mode
2.1 XFR.mode.idle
2.2 XFR.mode.send
2.3 XFR.mode.recv

3. Receive path
3.1 XFR.rx.progress
{
  "chunk": <uint32>,
  "offset": <uint64>,
  "eof": <bool>
}
3.2 XFR.rx.done
{
  "chunks": <uint32>,
  "bytes": <uint64>
}

4. Transmit path
4.1 XFR.tx.progress
{
  "chunk": <uint32>,
  "offset": <uint64>,
  "eof": <bool>
}
4.2 XFR.tx.done
{
  "chunks": <uint32>,
  "bytes": <uint64>
}

5. Chunk-level semantics
5.1 XFR.chunk.progress
{
  "index": <uint32>,
  "size": <uint32>
}
5.2 XFR.chunk.done
