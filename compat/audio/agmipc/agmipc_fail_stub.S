/* SPDX-License-Identifier: MIT
 *
 * Terminal degradation stub.  Every AGM slot is pre-initialised to this
 * address, so a trampoline can never branch through a null pointer even if it
 * runs before the shim constructor.  It reports once and returns -ENODEV.
 */
	.text
	.globl	agmipc_fail_stub
	.hidden	agmipc_fail_stub
	.type	agmipc_fail_stub, %function
	.p2align 2
agmipc_fail_stub:
	hint	#32
	stp	x29, x30, [sp, #-16]!
	bl	agmipc_report_unavailable
	ldp	x29, x30, [sp], #16
	movn	w0, #18			/* w0 = -19 == -ENODEV */
	ret
	.size	agmipc_fail_stub, .-agmipc_fail_stub

	.section	.note.GNU-stack,"",%progbits