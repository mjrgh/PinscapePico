// M0 Fault Dispatch
// By Dmitry.GR <me@dmitry.gr>
// Project page: https://dmitry.gr/?r=05.Projects&proj=27.%20m0FaultDispatch
//
// From the site:
// "The license is simple: This code is free for use in hobby and other non-commercial
// products. For commercial use, contact me <me@dmitry.gr>."


#include "m0FaultDispatch.h"
#include <stdbool.h>

#define STR2(x)			#x
#define STR(x)			STR2(x)



static uint32_t analyzeInstr16PrvUtilGetReg(const struct CortexExcFrame* frm, struct CortexPushedRegs *moreRegs, uint32_t regNo)
{
	switch (regNo) {
		case 0:		return frm->r0;
		case 1:		return frm->r1;
		case 2:		return frm->r2;
		case 3:		return frm->r3;
		case 4:		//fallthrough
		case 5:		//fallthrough
		case 6:		//fallthrough
		case 7:		return moreRegs->regs4_7[regNo - 4];
		case 8:		//fallthrough
		case 9:		//fallthrough
		case 10:	//fallthrough
		case 11:	return moreRegs->regs8_11[regNo - 8];
		case 12:	return frm->r12;
		case 13:	return (uintptr_t)(frm + 1);
		case 14:	return frm->lr;
		case 15:	return frm->pc + 4;
		default:	return 0xffffffff;
	}
}

static bool load8(uint32_t addr, uint32_t *dstP)
{
	uint32_t tmp;
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	ldrb	%1, [%2]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
		"	str		%1, [%3]	\n\t"
	:"=&l"(ret), "=&l"(tmp)
	:"l"(addr), "l"(dstP)
	:"cc", "memory");
	
	return ret;
}

static bool store8(uint32_t addr, uint32_t val)
{
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	strb	%2, [%1]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
	:"=&l"(ret)
	:"l"(addr), "l"(val)
	:"cc", "memory");
	
	return ret;
}

static bool load16(uint32_t addr, uint32_t *dstP)
{
	uint32_t tmp;
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	ldrh	%1, [%2]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
		"	str		%1, [%3]	\n\t"
	:"=&l"(ret), "=&l"(tmp)
	:"l"(addr), "l"(dstP)
	:"cc", "memory");
	
	return ret;
}

static bool store16(uint32_t addr, uint32_t val)
{
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	strh	%2, [%1]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
	:"=&l"(ret)
	:"l"(addr), "l"(val)
	:"cc", "memory");
	
	return ret;
}

static bool load32(uint32_t addr, uint32_t *dstP)
{
	uint32_t tmp;
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	ldr		%1, [%2]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
		"	str		%1, [%3]	\n\t"
	:"=&l"(ret), "=&l"(tmp)
	:"l"(addr), "l"(dstP)
	:"cc", "memory");
	
	return ret;
}

static bool store32(uint32_t addr, uint32_t val)
{
	bool ret;
	
	asm volatile(
		"	mov		%0, #1		\n\t"	//special formulation for our fault handler
		"	str		%2, [%1]	\n\t"
		"	b		1f			\n\t"
		"	mov		%0, #0		\n\t"
		"1:						\n\t"
	:"=&l"(ret)
	:"l"(addr), "l"(val)
	:"cc", "memory");
	
	return ret;
}

//we also get a pointer to the hiregs since we need to reset them before we call faultHandlerWithExcFrame()
// we get them in pushedRegs
void __attribute__((used)) analyzeInstr16(struct CortexExcFrame* frm, uint16_t instr, struct CortexPushedRegs *pushedRegs)
{
	static const uint8_t popCntTab[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};	//in a nibble
	static bool (*const accessR[])(uint32_t addr, uint32_t *dstP) = {load8, load16, load32};
	static bool (*const accessW[])(uint32_t addr, uint32_t val) = {store8, store16, store32};
	uint_fast8_t excCause = EXC_m0_CAUSE_UNCLASSIFIABLE, accessSzLog = 0;
	uint32_t addr, val, excExtraData = 0, ofst = 0, base = 0, i;
	bool testStore = false;
	
	switch (instr >> 11) {
		case 0b01000:
			if ((instr & 0x05c0) == 0x0500)			//cmp.hi with both lo regs
				excCause = EXC_m0_CAUSE_UNDEFINSTR16;
			if ((instr >> 8) == 0x47) {				//BX or BLX
				
				if (instr & 7)						//SBZ bits not Z
					excCause = EXC_m0_CAUSE_UNDEFINSTR16;
				else if ((instr & 0x78) == 0x78)	//PC
					excCause = EXC_m0_CAUSE_UNDEFINSTR16;
			}
			break;
		
		case 0b01001:	//load from literal pool (alignment guaranteed by instr)
			addr = ((frm->pc + 4) &~ 3) + ((instr & 0xff) << 2);
			if (!load32(addr, &val)) {
				
				excCause = EXC_m0_CAUSE_MEM_READ_ACCESS_FAIL;
				excExtraData = addr;
			}
			break;
		
		case 0b01010:	//load/store register
		case 0b01011:
			switch ((instr >> 9) & 0x07) {
				case 0b000:		//STR
					testStore = true;
					//fallthrough
				case 0b100:		//LDR
					accessSzLog = 2;
					break;
				case 0b001:		//STRH
					testStore = true;
					//fallthrough
				case 0b101:		//LDRH
				case 0b111:		//LDRSH
					accessSzLog = 1;
					break;
				case 0b010:		//STRB
					testStore = true;
					//fallthrough
				case 0b110:		//LDRB
				case 0b011:		//LDRSB
					accessSzLog = 0;
					break;
			}
			ofst = analyzeInstr16PrvUtilGetReg(frm, pushedRegs, (instr >> 6) & 0x07);
			goto load_store_check;
			
		case 0b01100:	//str imm
			testStore = true;
			//fallthrough
		case 0b01101:	//ldr imm
			accessSzLog = 2;
			goto load_store_check_get_imm;

		case 0b01110:	//strb imm
			testStore = true;
			//fallthrough
		case 0b01111:	//ldrb imm
			accessSzLog = 0;
			goto load_store_check_get_imm;

		case 0b10000:	//strh imm
			testStore = true;
			//fallthrough
		
		case 0b10001:	//ldrh imm
			accessSzLog = 1;
			goto load_store_check_get_imm;
		
load_store_check_get_imm:
			ofst = ((instr >> 6) & 0x1f) << accessSzLog;

load_store_check:
			base = analyzeInstr16PrvUtilGetReg(frm, pushedRegs, (instr >> 3) & 0x07);

load_store_check_have_base:
			addr = base + ofst;
			
			if (addr << (32 - accessSzLog)) {
				
				excCause = EXC_m0_CAUSE_DATA_UNALIGNED;
				excExtraData = addr;
			}
			else if (!accessR[accessSzLog](addr, &val)) {
				
				excCause = EXC_m0_CAUSE_MEM_READ_ACCESS_FAIL;
				excExtraData = addr;
			}
			else if (testStore && !accessW[accessSzLog](addr, val)) {
				
				excCause = EXC_m0_CAUSE_MEM_WRITE_ACCESS_FAIL;
				excExtraData = addr;
			}
			//if we have not faulted by now, we do not know why this load/store faulted
			break;
		
		case 0b10010:	//sp-based store
			testStore = true;
			//fallthrough
		
		case 0b10011:	//sp-based load
			ofst = (instr & 0xff) * 4;
			accessSzLog = 3;
			base = analyzeInstr16PrvUtilGetReg(frm, pushedRegs, 13);
			goto load_store_check_have_base;
		
		case 0b10111:
			if ((instr & 0x0700) == 0x0600)
				excCause = EXC_m0_CAUSE_BKPT_HIT;
			//push/pop are here too, but if they fail, we'd fail to stash and not ever get here
			break;
		
		case 0b11000:	//STMIA
			testStore = true;
			//fallthrough
		case 0b11001:	//LDMIA
			base = analyzeInstr16PrvUtilGetReg(frm, pushedRegs, (instr >> 8) & 0x07);
			if (base & 3) {							//unaligned base
				excExtraData = base;
				excCause = EXC_m0_CAUSE_DATA_UNALIGNED;
			}
			else if (!(instr & 0xff))				//LDM/STM with empty reg set
				excCause = EXC_m0_CAUSE_UNDEFINSTR16;
			else {
				i = popCntTab[instr & 0x0f] + popCntTab[(instr >> 4) & 0x0f];
				
				do {
					
					addr = base;
					base += 4;
					
					if (!load32(addr, &val)) {
						
						excCause = EXC_m0_CAUSE_MEM_READ_ACCESS_FAIL;
						excExtraData = addr;
					}
					else if (testStore && !store32(addr, val)) {
						
						excCause = EXC_m0_CAUSE_MEM_WRITE_ACCESS_FAIL;
						excExtraData = addr;
					}
					else
						continue;
					
					break;	//one fault found is enough
					
				} while (--i);
				//if we have not faulted by now, we do not know why this ldm/stm faulted
			}
			break;
		
		case 0b11011:
			if ((instr & 0x0700) == 0x0600)			//UDF
				excCause = EXC_m0_CAUSE_UNDEFINSTR16;
			break;
	}
	
	faultHandlerWithExcFrame(frm, excCause, excExtraData, pushedRegs);
}

void __attribute__((used,naked)) HardFault_Handler(void)
{
	asm volatile(
		
		//grab the appropriate SP
		"	mov   r0, lr										\n\t"
		"	lsr   r0, #3										\n\t"
		"	bcs   1f											\n\t"
		"	mrs   r0, msp										\n\t"
		"	b     2f											\n\t"
		"1:														\n\t"
		"	mrs   r0, psp										\n\t"
		"2:														\n\t"
		
		//check for ARM mode
		"	ldr   r1, [r0, #4 * 7]								\n\t"	//load pushed flags
		"	mov   r3, #1										\n\t"
		"	lsl   r3, #24										\n\t"	//T flag
		"	tst   r1, r3										\n\t"	//check for T bit
		"	bne   not_arm_mode									\n\t"	//if it is set, further testing to be done here
		"is_arm_mode:											\n\t"
		"	movs  r1, #" STR(EXC_m0_CAUSE_BAD_CPU_MODE) "		\n\t"
		"	b     call_handler_no_pushed_regs					\n\t"
		
		//check if re-entry
		"not_arm_mode:											\n\t"
		"	ldr   r2, [r0, #4 * 7]								\n\t"
		"	lsl   r2, #32 - 6									\n\t"
		"	lsr   r2, #32 - 6									\n\t"
		"	cmp   r2, #4										\n\t"
		"	bne   not_reentry									\n\t"
		
		//is re-entry from our own code only (should only be memory access issues)
		//our memory accesses are crafted specially and we detect that
		//we also assume here that Pc in our exclusive mode is valid
		"	ldr   r3, [r0, #4 * 6]								\n\t"	//exc.PC
		"	ldrh  r1, [r3, #2]									\n\t"	//should be a "B . +4" = 0xe000
		"	mov   r2, #0xe0										\n\t"
		"   lsl   r2, #8										\n\t"
		"	cmp   r1, r2										\n\t"
		"	bne   bug_in_classifier								\n\t"
		"	ldrh  r2, [r3, #4]									\n\t"	//should be a "MOV R?, #??" = 0x46f6
		"	lsr   r2, #11										\n\t"
		"	cmp   r2, #0x04										\n\t"
		"	bne   bug_in_classifier								\n\t"
		
		//re-entry from our own code - skip the load/store and the next instruction
		"	add   r3, #4										\n\t"
		"	str   r3, [r0, #4 * 6]								\n\t"	//exc.PC
		"	bx    lr											\n\t"
		
		//re-entry from our code but not a specially-instrumented load/store instr
		"bug_in_classifier:										\n\t"
		"	mov   r1, #" STR(EXC_m0_CAUSE_CLASSIFIER_ERROR) "	\n\t"
		"	b     call_handler_no_pushed_regs					\n\t"
		
		"access_fail_plus_4:									\n\t"
		"	add   r2, #2										\n\t"
		"access_fail_plus_2:									\n\t"
		"	add   r2, #2										\n\t"
		"access_fail:											\n\t"	//expects address in r2
		"	mov   r1, #" STR(EXC_m0_CAUSE_MEM_READ_ACCESS_FAIL) "\n\t"
		"call_handler_no_pushed_regs:							\n\t"
		"	push  {r4-r7, lr}									\n\t"
		"	mov   r4, r8										\n\t"
		"	mov   r5, r9										\n\t"
		"	mov   r6, r10										\n\t"
		"	mov   r7, r11										\n\t"
		"	push  {r4-r7}										\n\t"
		"	mov   r3, sp										\n\t"
		"	bl    faultHandlerWithExcFrame						\n\t"
		"	pop   {r4-r7}										\n\t"
		"	mov   r8, r4										\n\t"
		"	mov   r9, r5										\n\t"
		"	mov   r10, r6										\n\t"
		"	mov   r11, r7										\n\t"
		"	pop   {r4-r7, pc}									\n\t"
		
		//not re-entry, r3 still has T bit
		"not_reentry:											\n\t"
		//further checks will take place in another context - go there now
		"	ldr   r2, [r0, #4 * 6]								\n\t"	//exc.PC
		"   adr   r1, check_more_in_custom_mode					\n\t"	//stashed PC
		"	add   r3, r3, #4									\n\t"	//desired SR
		"	push  {r1, r3}										\n\t"
		"	mov   r1, r12										\n\t"	//stashed r12
		"	push  {r1, lr}										\n\t"	//and stashed lr
		"	push  {r0-r3}										\n\t"	//stashed r0..r3
		"	mov   r2, #0x0e										\n\t"
		"	mvn   r2, r2										\n\t"	//get an lr to go to (handler mode, main stack) -> 0xfffffff1
		"	bx    r2											\n\t"
		
		//check more in a safer space (r0 = exc; r2 = exc->pc; lr & sp set for direct return to original exc cause)
		
		".balign 4												\n\t"
		"check_more_in_custom_mode:								\n\t"
		"	cmp   r0, r0										\n\t"	//set Z
		"	ldrh  r1, [r2]										\n\t"	//load instr, on failure, branch is skipped too
		"	b     1f											\n\t"
		"	mov   r1, #1										\n\t"	//clear Z. only executed on access failure
		"1:														\n\t"	//Z flag is clear on failure, due to the mov above, which is skipped on a succesful read
		"	bne   access_fail									\n\t"
		"	lsr   r3, r1, #11									\n\t"
		"	cmp   r3, #0x1C										\n\t"
		"	bls   instr_is_16bits_long							\n\t"
		
		//let's read the second half of a 32-bit instr
		"instr_is_32_bits_long:									\n\t"
		"	mov   r12, r2										\n\t"	//save exc.pc
		"	cmp   r0, r0										\n\t"	//set Z
		"	ldrh  r2, [r2, #2]									\n\t"	//load instr part 2, on failure, branch is skipped too
		"	b     1f											\n\t"
		"	mov   r1, #1										\n\t"	//clear Z. only executed on access failure
		"1:														\n\t"	//Z flag is clear on failure, due to the mov above, which is skipped on a succesful read
		"	bne   access_fail_plus_2							\n\t"
		
		//32-bit instr. C-M0 has none that are valid AND can cause an exception, so this is definitely an UNDEF INSTR case
		"	mov   r1, #" STR(EXC_m0_CAUSE_UNDEFINSTR32) "		\n\t"
		"	b     call_handler_no_pushed_regs					\n\t"
		
		//it was a 16 bit instr. instr is in r1, pc is in r2
		"instr_is_16bits_long:									\n\t"
		//more triage will be done in C (r0 = excFrame, r1 = instr16, r2 = pc)
		"	push  {r4-r7, lr}									\n\t"
		"	mov   r4, r8										\n\t"
		"	mov   r5, r9										\n\t"
		"	mov   r6, r10										\n\t"
		"	mov   r7, r11										\n\t"
		"	push  {r4-r7}										\n\t"
		"	mov   r2, sp										\n\t"
		"	bl    analyzeInstr16								\n\t"
		"	pop   {r4-r7}										\n\t"
		"	mov   r8, r4										\n\t"
		"	mov   r9, r5										\n\t"
		"	mov   r10, r6										\n\t"
		"	mov   r11, r7										\n\t"
		"	pop   {r4-r7, pc}									\n\t"
		".ltorg													\n\t"
		:
		:
		: "cc", "memory", "r0", "r1", "r2", "r3", "r12" //yes gcc needs this list...
	);
}
