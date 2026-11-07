/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ERROR_INJECTION_H
#define __ASM_ERROR_INJECTION_H

#include <asm/ptrace.h>
#include <asm-generic/error-injection.h>

void override_function_with_return(struct pt_regs *regs);

#endif /* __ASM_ERROR_INJECTION_H */
