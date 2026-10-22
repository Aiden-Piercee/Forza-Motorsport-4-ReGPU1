#pragma once
// FM4 guest addresses the runtime hooks replace, from FM4's own default.xex (they differ from
// FM2's); each is a function the recompiled image defines as sub_XXXXXXXX.
//
// XAPI fibers: statically linked XDK code. The title's SwitchToFiber swaps the stack pointer and
// registers by hand, which recompiled code cannot follow (hooks/fm4/fibers.cpp replaces them
// with host fibers). Located by tools/findfibers.py and checked against the disassembly:
//   ConvertThreadToFiber  allocates the 0xA50-byte fiber object, sets KTHREAD+0x164
//   ConvertFiberToThread  clears KTHREAD+0x164, frees the object
//   CreateFiber           allocates the object plus a stack of at least 16 KB
//   DeleteFiber           on the current fiber it converts back and exits the thread
//   SwitchToFiber         0x82C85380 includes the register swap (0x82C85394..0x82C85788);
//                         callers reach it through the stub at 0x82C82C10 (b 0x82C85380), so
//                         there is no separate swap routine to hook. The start trampoline at
//                         0x82C82C20 is replaced by FiberMain.
#define FM4_XAPI_FIBER_ADDRESSES
#define FM4_XAPI_CONVERT_THREAD_TO_FIBER sub_82C829F0
#define FM4_XAPI_CONVERT_FIBER_TO_THREAD sub_82C82A80
#define FM4_XAPI_CREATE_FIBER            sub_82C82AD0
#define FM4_XAPI_DELETE_FIBER            sub_82C82BB0
#define FM4_XAPI_SWITCH_TO_FIBER         sub_82C85380
