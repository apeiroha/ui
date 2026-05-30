#include "ui.h"
#include "ui_internal.h"

/* All public API functions are directly implemented in their respective files:
 *   ui_core.c — Init, Fini, Run, Go, GoSized, Go1, Go1Sized, Yield, Sleep
 *   ui_chan.c — NewChan, ChanSend, ChanRecv, ChanTrySend, ChanTryRecv,
 *               ChanClose, ChanFree, SelectWait
 *   ui_sync.c — Mutex*, Cond*
 *   ui_io.c   — Read, Write, Open
 */

/* This file exists as a compilation unit anchor for ui.h inclusion
 * and to satisfy the build system. All implementations are in the
 * component files above.
 */
