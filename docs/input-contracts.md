# Raw client input contracts

04a adds device-free input state in EngineClient. The
[04 checkpoint plan](roadmap/window-input-plan.md) separates these CPU contracts
from native GLFW integration in 04b and frame-loop consumption in 04c. Core and
Server do not include or link client input. The raw state neither changes a clock
nor pauses or mutates an authoritative world.

## Ownership and injection

`shark::client::InputState` belongs to one client/window owner. All calls are
serialized on its controlling thread; there is no synchronization or worker.
Tests and the future native adapter use the same `on_key`, `on_mouse_button`,
`on_cursor`, `on_scroll`, `on_text` and `on_focus` methods. There is no hidden OS
polling, event bus, provider SDK or dependency on a display.

All ingestion, capture and query methods are `noexcept` and perform no allocation,
logging, blocking or user callbacks. State uses fixed inline arrays. Ordinary
events take constant work; focus loss and capture visit the fixed catalog.
`InputEventResult` reports accepted, ignored-unfocused, invalid-argument/transition
or overflow outcomes without constructing an allocating error inside a future C
callback. The 04b adapter must handle or translate failures into the existing
owned diagnostics outside that callback. These C++ enums are not a versioned
authoring or gameplay-wire protocol.

## Controls and capture boundaries

`Key` names 120 engine-owned keyboard locations, including letters, digits,
navigation, function/keypad keys and separate left/right modifiers. `MouseButton`
names eight buttons. Neither enum uses native GLFW values, characters or durable
serialized IDs; 04b owns the explicit native mapping. `unknown`, `count` and
out-of-range casts are invalid. Snapshot queries return neutral state for an
invalid ID; `valid()` distinguishes invalid queries from inactive valid controls.

Each button stores `held`, `pressed`, `released` and `repeated`. A new press sets
held/pressed; release clears held and sets released. Duplicate presses/releases
are accepted without creating an extra transition. Keyboard repeat requires an
already-held key and never manufactures a fresh press. Mouse repeat is invalid.

`capture()` returns an owned `InputSnapshot` and clears only interval data:
edge/repeat flags, focus-transition flags, scroll, text and overflow indicators.
Held state, current focus and a valid cursor position survive. Copying or changing
a snapshot cannot change its source state. Repeated captures without new events
therefore expose held controls with no repeated one-shot edges.

Events apply in injection order, while snapshots coalesce multiple occurrences.
A press/release within one interval preserves both edges even though held is false;
a release/press can preserve both while held is true. A snapshot is not an ordered
key-event log or a count of taps. Text order is retained separately. Frame/tick
latching, modifier-aware action routing, UI capture and action bindings need their
own explicit integration; callers must not consume the same pressed edge once per
catch-up tick. 04c defines frame/tick consumption using the existing schedulers.

## Focus cancellation

The initial state is unfocused. The owner must explicitly report focus before
input is accepted. Malformed identifiers/actions/coordinates/scalars are rejected
before the unfocused-ignore decision, with existing state unchanged.

Focus loss is a cancellation boundary:

- Release every held keyboard key and mouse button, retaining existing release
  edges so consumers can observe neutralization.
- Cancel unconsumed pressed/repeated flags, text and scroll, preventing an action
  received just before switching windows from firing afterward.
- Invalidate the cursor and mark focus lost. Retain overflow indicators until
  capture so a focus change cannot hide a prior overflow.

Valid input while unfocused is ignored. Regaining focus does not restore held
controls or a cursor position: a new press/position is required. Repeat of a key
that is no longer held is invalid. Duplicate focus notifications do not erase
pending state. If loss and regain occur before capture, both transition flags
are visible with the latest focus state; only newly accepted events after regain
can produce positive actions.

An online input/session layer will translate neutral state into its defined
released-input message. No such network protocol is implemented here. Focus loss
does not grant a client control over server time; offline pause remains 04c work.

## Cursor, scroll and text bounds

Cursor observations use finite double-precision content-area coordinates: origin
top-left, +x right, +y down. Negative/outside coordinates are allowed. They are not
framebuffer pixels or relative/raw mouse-motion deltas. Native scale conversion,
framebuffer sizes, cursor capture and high-DPI smoke evidence belong to 04b.

Scroll uses fractional native units on two axes. Nonfinite arguments reject
without mutation. Finite deltas accumulate until capture; if either sum becomes
nonfinite, both new components reject atomically with overflow reported. The
last valid sums survive and a sticky `scroll_overflow` flag records the event.

Text accepts Unicode scalar values, separate from key IDs: U+0000 through
U+10FFFF excluding surrogate values U+D800..U+DFFF. The snapshot has 64 inline
`char32_t` entries and an explicit length; it is not null-terminated UTF-8 and may
contain U+0000. Noncharacters are valid scalar values. Composition/IME, grapheme
editing, clipboard and character-to-key mapping are not implemented.

On text overflow, the accepted prefix is retained and a sticky `text_overflow`
flag is set. The caller receives overflow for the rejected scalar. Overflow
does not disable later keyboard/mouse releases or focus cancellation; capture
clears the flags and opens the next bounded text interval.

## Verification boundary

`shark.client.input` uses deterministic injection with seed 1337 and a 15-second
CTest timeout. It is registered only when EngineClient is built; headless Server
configurations exclude its source, header and test. CPU client tests can execute
in the existing minimal WSL userspace without a display; that proves the contract,
not native Linux window support. The
[04a handoff](handoffs/04a-input-contracts.md) records exact results and limitations.
No editor UI, authored command, real keyboard interaction or GLFW window is claimed.
