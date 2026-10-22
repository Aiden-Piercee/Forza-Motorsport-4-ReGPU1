#pragma once
// FM4 guest addresses the runtime hooks replace. Take them from FM4's own default.xex (they
// differ from FM2's); each is a function the recompiled image defines as sub_XXXXXXXX.
//
// XAPI fibers: statically linked XDK code. The title's SwitchToFiber swaps the stack pointer and
// registers by hand, which recompiled code cannot follow (hooks/fm4/fibers.cpp replaces them
// with host fibers). Find them by the same shapes as FM2's (src/hooks/fm2/fibers.cpp):
//   ConvertThreadToFiber(data)    stores the fiber in KTHREAD+0x164
//   ConvertFiberToThread()
//   CreateFiber(stackSize, start, data)
//   DeleteFiber(fiber)
//   SwitchToFiber(fiber)
//   the register-swap routine SwitchToFiber jumps to (must never run)
//
// Define FM4_XAPI_FIBER_ADDRESSES once all six are filled in.
// #define FM4_XAPI_FIBER_ADDRESSES
// #define FM4_XAPI_CONVERT_THREAD_TO_FIBER sub_XXXXXXXX
// #define FM4_XAPI_CONVERT_FIBER_TO_THREAD sub_XXXXXXXX
// #define FM4_XAPI_CREATE_FIBER            sub_XXXXXXXX
// #define FM4_XAPI_DELETE_FIBER            sub_XXXXXXXX
// #define FM4_XAPI_SWITCH_TO_FIBER         sub_XXXXXXXX
// #define FM4_XAPI_FIBER_SWAP_ROUTINE      sub_XXXXXXXX
