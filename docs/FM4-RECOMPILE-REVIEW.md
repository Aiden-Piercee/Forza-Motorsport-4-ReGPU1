# FM4 recompile review (milestone 1)

This explains every entry left in the fallthrough lists and the cross-check after
the recompile at 202eea9. Only addresses and the reasons for them are recorded
here. Each one was checked in the disassembly on the machine that has the game.

## Results

| Image | Functions | Jump tables | Unresolved bctrs | Unrecognized instructions | Continue in next function | Link check |
|---|---|---|---|---|---|---|
| default.xex | 50425 | 394 | 2 | 0 | 132 | ok |
| XMediaFacade_default.xex | 3303 | 26 | 0 | 0 | 9 | ok |
| SpeechFacade_default.xex | 6846 | 20 | 0 | 0 | 7 | ok |

The two unresolved bctrs in default.xex, 82AC8F24 and 82AC8F5C, are tail calls
through a function pointer loaded from a table of structures. They are not jump
tables. The recompiler emits them as indirect calls.

## Fallthroughs

A function whose last instruction can fall through into the next function calls
that function and returns (the "continue" column above). That leaves 161
entries in `out/<name>.fallthrough.txt`, and none of them is a real
fallthrough:

- **The data at the end of each code section (43).** These are default.xex
  82D4BBC8–82D4BBF8, XMF 88244E8C–88244EE8 and SPH 8920678C–892067DC. Each
  block starts with `EVH\0` and holds structured words. It is not code, and it
  is also the source of the "Unable to decode instruction" log lines.
- **A trap (1).** 82C4E038 is a single `twi 31,r0,22`, an unconditional trap.
- **Calls that never return (117).** The function ends in a `bl` that never
  returns to its caller, and after it comes padding, the next function's
  exception-handler data, or the next function:

  | Target | Callers | Why it doesn't return |
  |---|---|---|
  | 823A5730 | 60 | Throw helper: calls an optional handler, then terminate (82765980) |
  | 82765980 | 8 | terminate: calls `KeBugCheck` |
  | 8276B9C8 | 19 | Raises a C++ exception (`RtlRaiseException`, non-continuable) |
  | 82777CB8 | 7 | Formats an error, then throws (8276B9C8) |
  | 8275E590 | 4 | Tail call to `XamLoaderLaunchTitle` |
  | 82768700 | 2 | longjmp |
  | 82C82010, SPH 891F0E38 | 2 | ExitThread: calls `ExTerminateThread` |
  | 82CA7E58 | 1 | png_error: ends in 827668D8, which never returns (below) |
  | XMF 881F0848, SPH 89202D98 | 2 | The facades' own C++ throw (same shape as 8276B9C8) |
  | default.xex 8276A148, 82769F70, 8276A138, 82772FF0, 824266C0; XMF 88050D60, 880524A0; SPH 890A0A10, 890A21C8 (2), 890A0BE8, 890A2008 | 12 | CRT abort and terminate wrappers. No `blr` is reachable in the body or in anything it tail-calls |

## Cross-check against the earlier function list

`tools/fm4_crosscheck.py` reports no vtable targets, no runtime crash targets
and no unresolved targets. In XMF and SPH it finds only `.pdata` family entries.
In default.xex, the categories that could matter turned out to need nothing:

- **29 vcall-tail and vcall-thunk entries.** Each one starts right after an
  unconditional `bctr`, `blr` or `b`. But each one is reached by one
  conditional branch near the start of the function that holds it: it is a case
  body that ends in its own tail call. None of these addresses appears in any
  data section, and no code builds one. The functions that hold them are the
  ones in the vtables. XenonRecomp already emits them as labels. A `functions`
  entry would cut the holding function, so they get none. 82B0E494 and
  825D173C are internal branch targets in the same way.
- **83 re-entry continuations.** All of them are return addresses: 76 come
  after a `bl` and 7 after a `bctrl`. None of them appears in data, none is
  built in a register, and none is in a function that has exception data, so
  they are not C++ catch continuations. Returns use the host stack, so they need
  no entry. Three of them are real resume points, and the runtime handles them:
  - two after the call to SwitchToFiber (82C82C10), handled by the fiber
    replacements;
  - one after setjmp (82768B30), handled through `setjmp_address` and
    `longjmp_address` in `config/fm4.toml`.
- **The safe tail target** is the SwitchToFiber entry in `config/fm4.toml`.
