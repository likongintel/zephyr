/* loApicIntr.c - Intel Pentium[234] Local APIC/xAPIC driver */

/*
 * Copyright (c) 1984-2008, 2011-2015 Wind River Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Wind River Systems nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
DESCRIPTION
This module is a driver for the local APIC/xAPIC (Advanced Programmable
Interrupt Controller) in P6 (PentiumPro, II, III) family processors
and P7 (Pentium4) family processors.  The local APIC/xAPIC is included
in selected P6 (PentiumPro, II, III) and P7 (Pentium4) family processors.
Beginning with the P6 family processors, the presence or absence of an
on-chip local APIC can be detected using the CPUID instruction.  When the
CPUID instruction is executed, bit 9 of the feature flags returned in the
EDX register indicates the presence (set) or absence (clear) of an on-chip
local APIC.

The local APIC performs two main functions for the processor:
 - It processes local external interrupts that the processor receives at its
   interrupt pins and local internal interrupts that software generates.
 - It communicates with an external IO APIC
   chip.  The external IO APIC receives external interrupt events from
   peripheral and direct them to the local APIC.  The IO APIC is
   part of Intel's system chip set.
The local APIC controls the dispatching of interrupts (to its associated
processor) that it receives either locally or from the IO APIC.  It provides
facilities for queuing, nesting and masking of interrupts.  It handles the
interrupt delivery protocol with its local processor and accesses to APIC
registers.
A timer on the local APIC allows local generation of interrupts, and
local interrupt pins permit local reception of processor-specific interrupts.
The local APIC can be disabled and used in conjunction with a standard 8259A
style interrupt controller.  Disabling the local APIC can be done in hardware
for the Pentium processors or in software for the P6 and P7 (Pentium4) family
processors.

The local APIC in the Pentium4 processors (called the xAPIC) is an extension
of the local APIC found in the P6 family processors.  The primary difference
between the APIC architecture and xAPIC architecture is that with Pentium4
processors, the local xAPICs and IO xAPIC communicate with one another through
the processors system bus; whereas, with the P6 family processors, communication
between the local APICs and the IO APIC is handled through a dedicated 3-wire
APIC bus.  Also, some of the architectural features of the local APIC have been
extended and/or modified in the local xAPIC.

The base address of the local APIC and IO APIC is taken from the platform.
It uses LOAPIC_BASE_ADRS and IOAPIC_BASE_ADRS.
This driver contains three routines for use.  They are:
_loapic_init() initializes the Local APIC for the interrupt mode chosen.
_loapic_enable()/disable() enables / disables the Local APIC.

Local APIC is used in the Virtual Wire Mode: delivery mode ExtINT.

Virtual Wire Mode is one of three interrupt modes defined by the MP
specification.  In this mode, interrupts are generated by the 8259A equivalent
PICs (if present) and delivered to the Boot Strap Processor by
the local APIC that is programmed to act as a "virtual Wire"; that
is, the local APIC is logically indistinguishable from a hardware
connection.  This is a uniprocessor compatibility mode.

The local and IO APICs support interrupts in the range of 32 to 255.
Interrupt priority is implied by its vector, according to the following
relationship:  "priority = vector / 16".
Here the quotient is rounded down to the nearest integer value to determine
the priority, with 1 being the lowest and 15 is the highest.  Because vectors
0 through 31 are reserved for exclusive use by the processor, the priority of
user defined interrupts range from 2 to 15.  A value of 15 in the Interrupt
Class field of the Task Priority Register (TPR) will mask off all interrupts,
which require interrupt service.
The P6 family processor's local APIC includes an in-service entry and a holding
entry for each priority level.  To avoid losing interrupts, software should
allocate no more than 2 interrupt vectors per priority.  The Pentium4 processor
expands this support of all acceptance of two interrupts per vector rather than
per priority level.

INCLUDE FILES: loapic.h
 */

#include <nanokernel.h>
#include <arch/cpu.h>
#include <stdint.h>

#include "board.h"
#include <toolchain.h>
#include <sections.h>
#include <drivers/loapic.h> /* public API declarations */

/* IA32_APIC_BASE MSR Bits */

#define LOAPIC_BASE_MASK 0xfffff000     /* LO APIC Base Addr mask */
#define LOAPIC_GLOBAL_ENABLE 0x00000800 /* LO APIC Global Enable */

/* Local APIC ID Register Bits */

#define LOAPIC_ID_MASK 0x0f000000 /* LO APIC ID mask */

/* Local APIC Version Register Bits */

#define LOAPIC_VERSION_MASK 0x000000ff /* LO APIC Version mask */
#define LOAPIC_MAXLVT_MASK 0x00ff0000  /* LO APIC Max LVT mask */
#define LOAPIC_PENTIUM4 0x00000014     /* LO APIC in Pentium4 */
#define LOAPIC_LVT_PENTIUM4 5	  /* LO APIC LVT - Pentium4 */
#define LOAPIC_LVT_P6 4		       /* LO APIC LVT - P6 */
#define LOAPIC_LVT_P5 3		       /* LO APIC LVT - P5 */

/* Local APIC Vector Table Bits */

#define LOAPIC_VECTOR 0x000000ff /* vectorNo */
#define LOAPIC_MODE 0x00000700   /* delivery mode */
#define LOAPIC_FIXED 0x00000000  /* delivery mode: FIXED */
#define LOAPIC_SMI 0x00000200    /* delivery mode: SMI */
#define LOAPIC_NMI 0x00000400    /* delivery mode: NMI */
#define LOAPIC_EXT 0x00000700    /* delivery mode: ExtINT */
#define LOAPIC_IDLE 0x00000000   /* delivery status: Idle */
#define LOAPIC_PEND 0x00001000   /* delivery status: Pend */
#define LOAPIC_HIGH 0x00000000   /* polarity: High */
#define LOAPIC_LOW 0x00002000    /* polarity: Low */
#define LOAPIC_REMOTE 0x00004000 /* remote IRR */
#define LOAPIC_EDGE 0x00000000   /* trigger mode: Edge */
#define LOAPIC_LEVEL 0x00008000  /* trigger mode: Level */

/* Local APIC Spurious-Interrupt Register Bits */

#define LOAPIC_ENABLE 0x100	/* APIC Enabled */
#define LOAPIC_FOCUS_DISABLE 0x200 /* Focus Processor Checking */

/* Local Vector's lock-unlock macro used in loApicIntLock/Unlock */

#define LOCKED_TIMER 0x01
#define LOCKED_PMC 0x02
#define LOCKED_LINT0 0x04
#define LOCKED_LINT1 0x08
#define LOCKED_ERROR 0x10
#define LOCKED_THERMAL 0x20

/* Interrupt Command Register: delivery mode and status */

#define MODE_FIXED 0x0     /* delivery mode: Fixed */
#define MODE_LOWEST 0x1    /* delivery mode: Lowest */
#define MODE_SMI 0x2       /* delivery mode: SMI */
#define MODE_NMI 0x4       /* delivery mode: NMI */
#define MODE_INIT 0x5      /* delivery mode: INIT */
#define MODE_STARTUP 0x6   /* delivery mode: StartUp */
#define STATUS_PEND 0x1000 /* delivery status: Pend */

/* MP Configuration Table Entries */

#define MP_ENTRY_CPU 0	 /* Entry Type: CPU */
#define MP_ENTRY_BUS 1	 /* Entry Type: BUS */
#define MP_ENTRY_IOAPIC 2      /* Entry Type: IO APIC */
#define MP_ENTRY_IOINTERRUPT 3 /* Entry Type: IO INT */
#define MP_ENTRY_LOINTERRUPT 4 /* Entry Type: LO INT */

/* Extended MP Configuration Table Entries */

#define EXT_MP_ENTRY_SASM 128  /* Entry Type: System Address Space Map */
#define EXT_MP_ENTRY_BHD 129   /* Entry Type: Bus Hierarchy Descriptor */
#define EXT_MP_ENTRY_CBASM 130 /* Entry Type: Comp Address Space Modifier */

/* MP Configuration Table CPU Flags */

#define MP_CPU_FLAGS_BP 0x02

/* IMCR related bits */

#define IMCR_ADRS 0x22       /* IMCR addr reg */
#define IMCR_DATA 0x23       /* IMCR data reg */
#define IMCR_REG_SEL 0x70    /* IMCR reg select */
#define IMCR_IOAPIC_ON 0x01  /* IMCR IOAPIC route enable */
#define IMCR_IOAPIC_OFF 0x00 /* IMCR IOAPIC route disable */


/**
 *
 * @brief Initialize the Local APIC or xAPIC
 *
 * This routine initializes Local APIC or xAPIC.
 *
 * @return N/A
 *
 */

void _loapic_init(void)
{
	int32_t loApicMaxLvt; /* local APIC Max LVT */

	/* enable the Local APIC */

	_loapic_enable();

	loApicMaxLvt = (*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_VER) &
			LOAPIC_MAXLVT_MASK) >>
		       16;

	/* reset the DFR, TPR, TIMER_CONFIG, and TIMER_ICR */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_DFR) = (int)0xffffffff;
	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TPR) = (int)0x0;
	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER_CONFIG) = (int)0x0;
	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER_ICR) = (int)0x0;

	/* program Local Vector Table for the Virtual Wire Mode */

	/* set LINT0: extInt, high-polarity, edge-trigger, not-masked */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_LINT0) =
		(*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_LINT0) &
		 ~(LOAPIC_MODE | LOAPIC_LOW | LOAPIC_LEVEL | LOAPIC_LVT_MASKED)) |
		(LOAPIC_EXT | LOAPIC_HIGH | LOAPIC_EDGE);

	/* set LINT1: NMI, high-polarity, edge-trigger, not-masked */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_LINT1) =
		(*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_LINT1) &
		 ~(LOAPIC_MODE | LOAPIC_LOW | LOAPIC_LEVEL | LOAPIC_LVT_MASKED)) |
		(LOAPIC_NMI | LOAPIC_HIGH | LOAPIC_EDGE);

	/* lock the Local APIC interrupts */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER) = LOAPIC_LVT_MASKED;
	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_ERROR) = LOAPIC_LVT_MASKED;

	if (loApicMaxLvt >= LOAPIC_LVT_P6)
		*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_PMC) = LOAPIC_LVT_MASKED;

	if (loApicMaxLvt >= LOAPIC_LVT_PENTIUM4)
		*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_THERMAL) =
			LOAPIC_LVT_MASKED;

	/* discard a pending interrupt if any */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_EOI) = 0;
}

/**
 *
 * @brief Enable the Local xAPIC
 *
 * This routine enables the Local xAPIC.
 *
 * @return N/A
 */

void _loapic_enable(void)
{
	int32_t oldLevel = irq_lock(); /* LOCK INTERRUPTS */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_SVR) |= LOAPIC_ENABLE;

	irq_unlock(oldLevel); /* UNLOCK INTERRUPTS */
}

/**
 *
 * @brief Disable the Local xAPIC
 *
 * This routine disables the Local xAPIC.
 *
 * @return N/A
 */

void _loapic_disable(void)
{
	int32_t oldLevel = irq_lock(); /* LOCK INTERRUPTS */

	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_SVR) &= ~LOAPIC_ENABLE;

	irq_unlock(oldLevel); /* UNLOCK INTERRUPTS */
}

/**
 *
 * @brief  send EOI (End Of Interrupt) signal to Local APIC
 *
 * This routine sends an EOI signal to the Local APIC's interrupting source.
 *
 * @return N/A
 */

void _loapic_eoi(unsigned int irq)
{
	ARG_UNUSED(irq);
	*(volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_EOI) = 0;
}

/**
 *
 * @brief Set the vector field in the specified RTE
 *
 * This routine is utilized by the interrupt controller's _SysIntVecAlloc()
 * routine (which exists to support the irq_connect() API).  Once
 * a vector has been allocated, this routine is invoked to update the LVT
 * entry associated with <irq> with the vector.
 *
 * @return N/A
 */

void _loapic_int_vec_set(unsigned int irq, /* IRQ number of the interrupt */
				  unsigned int vector /* vector to copy into the LVT */
				  )
{
	volatile int *pLvt; /* pointer to local vector table */
	int32_t oldLevel;   /* previous interrupt lock level */

	/*
	 * The following mappings are used:
	 *
	 *   IRQ0 -> LOAPIC_TIMER
	 *   IRQ1 -> LOAPIC_THERMAL
	 *   IRQ2 -> LOAPIC_PMC
	 *   IRQ3 -> LOAPIC_LINT0
	 *   IRQ4 -> LOAPIC_LINT1
	 *   IRQ5 -> LOAPIC_ERROR
	 *
	 * It's assumed that LVTs are spaced by 0x10 bytes
	 */

	pLvt = (volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER + (irq * 0x10));

	/* update the 'vector' bits in the LVT */

	oldLevel = irq_lock();
	*pLvt = (*pLvt & ~LOAPIC_VECTOR) | vector;
	irq_unlock(oldLevel);
}

/**
 *
 * @brief Enable an individual LOAPIC interrupt (IRQ)
 *
 * This routine clears the interrupt mask bit in the LVT for the specified IRQ
 *
 * @return N/A
 */

void _loapic_irq_enable(unsigned int irq /* IRQ number of
						     the interrupt */
				 )
{
	volatile int *pLvt; /* pointer to local vector table */
	int32_t oldLevel;   /* previous interrupt lock level */

	/*
	 * See the comments in _LoApicLvtVecSet() regarding IRQ to LVT mappings
	 * and ths assumption concerning LVT spacing.
	 */

	pLvt = (volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER + (irq * 0x10));

	/* clear the mask bit in the LVT */

	oldLevel = irq_lock();
	*pLvt = *pLvt & ~LOAPIC_LVT_MASKED;
	irq_unlock(oldLevel);
}

/**
 *
 * @brief Disable an individual LOAPIC interrupt (IRQ)
 *
 * This routine clears the interrupt mask bit in the LVT for the specified IRQ
 *
 * @return N/A
 */

void _loapic_irq_disable(unsigned int irq /* IRQ number of the
						      interrupt */
				  )
{
	volatile int *pLvt; /* pointer to local vector table */
	int32_t oldLevel;   /* previous interrupt lock level */

	/*
	 * See the comments in _LoApicLvtVecSet() regarding IRQ to LVT mappings
	 * and ths assumption concerning LVT spacing.
	 */

	pLvt = (volatile int *)(LOAPIC_BASE_ADRS + LOAPIC_TIMER + (irq * 0x10));

	/* set the mask bit in the LVT */

	oldLevel = irq_lock();
	*pLvt = *pLvt | LOAPIC_LVT_MASKED;
	irq_unlock(oldLevel);
}
