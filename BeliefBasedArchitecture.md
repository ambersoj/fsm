# MPP Belief-Based Architecture — Integration Guide

This document describes the **current incarnation of MPP** centered around **Beliefs**, **commit()**, **BLS**, and **HUD**. It is intended as a practical API and design guide for building new components (e.g., XFR) that fit naturally into the system.

---

## 1. Mental Model (Read This First)

MPP has crossed a boundary:

* **Packets are no longer the story**
* **State is the story**

Beliefs are *facts about the world* that become true and remain true.

A belief is **not**:

* an event
* a log line
* a timestamp

A belief **is**:

* an assertion of state
* monotonic knowledge
* something a human can reason about

> Revision is not something you cause.
> It falls out of belief accumulation.

---

## 2. Core Components and Roles

### FSM (Brain)

* Decides *when* transitions occur
* Commits beliefs about logical state
* Does **not** store history

### NET (Doer)

* Interacts with the real world (network)
* Commits beliefs about observable facts
* Never predicts outcomes

### BLS (Memory)

* Stores the latest belief per subject
* Maintains revision counter
* Answers "what is currently true?"
* Re-exposes state on request

### HUD (Observer)

* Displays belief state
* Does not listen to BUS events
* Polls BLS for snapshots

---

## 3. Belief Anatomy

A belief has four fields:

```json
{
  "component": "NET",
  "subject": "NET.rx_done",
  "polarity": true,
  "context": {}
}
```

### Rules

* `component` is the owner
* `subject` **must** be namespaced (`NET.*`, `FSM.*`, etc.)
* `polarity` is usually `true`
* `context` is optional, structured detail

Beliefs are **monotonic**:

* Once true, they stay true
* No retraction (yet)

---

## 4. commit() API

Every MPP component may call:

```cpp
commit("NET.rx_done", true);
```

### commit() Guarantees

* Enforces namespace ownership
* Suppresses duplicates (same subject + polarity)
* Emits belief to BUS

### commit() Is NOT

* A log statement
* A heartbeat
* A progress meter

> Ask: *If HUD showed this, would a human say "ah — that explains something"?*

---

## 5. When to Commit a Belief

### Good Commit Points

* Resource created

  * `NET.libnet.tx.created`
  * `NET.pcap.rx.created`

* Action completed

  * `NET.tx.fire.committed`
  * `NET.rx_done`

* Logical window closed

  * `FSM.rx.window.closed`

### Bad Commit Points

* Constructor ran
* Loop iteration
* Poll tick
* Sample taken

If it’s noisy, don’t commit it.

---

## 6. Revision Semantics

Each belief increments BLS revision.

Revision means:

* "Something new became true"
* NOT "something happened at time T"

Over time revision becomes:

* a logical clock
* a causality marker
* a time-travel index

No timestamps required.

---

## 7. BLS Responsibilities

BLS:

* Listens to BUS
* Ignores beliefs with `_via == "BLS"`
* Stores `current_[subject] = belief`
* Increments revision

On snapshot (`read:true`), BLS returns:

```json
{
  "component": "BLS",
  "revision": 7,
  "beliefs": [ ... ]
}
```

BLS is **memory**, not messaging.

---

## 8. HUD Responsibilities

HUD:

* Does NOT listen to BUS
* Periodically sends:

```json
{ "read": true }
```

* Accepts BLS snapshots only
* Renders beliefs as truth

HUD never deduplicates — BLS already did.

---

## 9. Adding a New Component (XFR Example)

To integrate XFR cleanly:

### 1. Pick Belief Subjects

Examples:

* `XFR.mode.send`
* `XFR.chunk.ready`
* `XFR.send.done`
* `XFR.recv.done`

### 2. Commit Only Stable Facts

```cpp
commit("XFR.send.done", true);
```

Avoid:

* per-chunk commits
* per-byte commits

### 3. Let FSM Coordinate

FSM should react to:

* `XFR.chunk.ready`
* `XFR.recv.done`

XFR should not encode control logic.

---

## 10. Common Smells

🚫 `commit("*.started")` in constructor

🚫 committing in polling loops

🚫 beliefs that mirror registers exactly

🚫 using belief stream as logging

---

## 11. Where This Is Going

Soon (optional):

* revision-gated HUD refresh
* belief retraction (false polarity)
* timeout beliefs (`NET.rx.timeout`)

Later:

* causal replay
* belief diffing
* state forensics

---

## 12. Final Truth

This system works because:

* FSM thinks
* NET observes
* BLS remembers
* HUD shows

Packets fade away.
Beliefs remain.

You are no longer debugging events.
You are reasoning about reality.
