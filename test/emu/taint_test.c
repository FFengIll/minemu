
#include <sys/mman.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "debug.h"
#include "codeexec.h"
#include "taint.h"
#include "opcodes.h"

long taint_tmp[1];

long imm_at(char *addr, long size)
{
	long imm=0;
	memcpy(&imm, addr, size);
	if (size == 1)
		return *(signed char*)&imm;
	else
		return imm;
}

void imm_to(char *dest, long imm)
{
	memcpy(dest, &imm, sizeof(long));
}

int die() { return -1; }

void fill(char *bin, int size)
{
	int i;
	for (i=0; i<size; i++)
		bin[i]=(char)i;
}

long mem_test[8], mem_backup[8], taintmem_test[8], taintmem_backup[8];
char fx_test[512]   __attribute__ ((aligned (16))),
     fx_backup[512] __attribute__ ((aligned (16))),
     fx_orig[512]   __attribute__ ((aligned (16)));
long regs_test[9], regs_backup[9];
long offset;
int err = EXIT_SUCCESS;
long *taint_regs = ((long *)&fx_test[256]);
char opcode[256]; int oplen;

long regs_orig[] =
{
	(long)mem_test,
	(long)mem_test+4,
	(long)mem_test+8,
	(long)mem_test+12,
	(long)mem_test+16,
	(long)mem_test+20,
	(long)mem_test+24,
	(long)mem_test+28,
	0x09000246, /* flags */
};

long scratch_orig[] =
{
	0x15796276,
	0x97840632,
	0x85498748,
	0x45745459,
};

long taint_orig[] =
{
	0x01020408,
	0x02040810,
	0x04081020,
	0x08102040,
	0x10204080,
	0x20408001,
	0x40800102,
	0x80010204,
};

long taintmem_orig[] =
{
	0x00001111,
	0x00110011,
	0x11110000,
	0x01010101,
	0x11001100,
	0x11000011,
	0x10101010,
	0x00111100,
};

char *mrm_tests[] = 
{
	"\x00",
	"\x01",
	"\x02",
	"\x03",
	"\x04\x24",
	"\x45\x00",
	"\x06",
	"\x07",
	"\xC0",
	"\xC1",
	"\xC2",
	"\xC3",
	"\xC4",
	"\xC5",
	"\xC6",
	"\xC7",
	NULL
};

int is_memop(char *mrm)
{
    return (mrm[0] & 0xC0) != 0xC0;
}

int mrm_len(char *mrm)
{
	int len = 1;
	if ( ((mrm[0]&0xC0) != 0xC0) && ((mrm[0]&0x07) == 0x04) ) len+=1;
	if ( ((mrm[0]&0xC7) == 0x04) && ((mrm[1]&0x07) == 0x05) ) len+=4;
	if   ((mrm[0]&0xC7) == 0x05)                              len+=4;
	if   ((mrm[0]&0xC0) == 0x40)                              len+=1;
	if   ((mrm[0]&0xC0) == 0x80)                              len+=4;
	return len;
}

char *do_lea(char *m)
{
	int mlen = mrm_len(m);
    char op[mlen+1];
    long regs[9];
	memcpy(regs, regs_test, 9*sizeof(long));

    op[0] = '\x8D';
    memcpy(&op[1], m, mlen);
	op[1] &= ~0x38;

    codeexec((char *)op, mlen+1, (long *)regs);
    return (char *)regs[0];
}

void setup(void)
{
	fill((char*)mem_test, 32);
	memcpy(taintmem_test, taintmem_orig, 32);
	memcpy(fx_test, fx_orig, 512);
	memcpy(regs_test, regs_orig, 9*sizeof(long));
}

void backup(void)
{
	memcpy(mem_backup, mem_test, 32);
	memcpy(taintmem_backup, taintmem_test, 32);
	memcpy(fx_backup, fx_test, 512);
	memcpy(regs_backup, regs_test, 9*sizeof(long));
	setup();
}

void diff(void)
{
	int e=0;
	if (bcmp(regs_backup, regs_test, 36)) 
	{
		printhex_diff(regs_backup, 36, regs_test, 36, 4);
		e=err = EXIT_FAILURE;
	}
	if (bcmp(mem_backup, mem_test, 32)) 
	{
		printhex_diff(mem_backup, 32, mem_test, 32, 1);
		e=err = EXIT_FAILURE;
	}
	if (bcmp(taintmem_backup, taintmem_test, 32)) 
	{
		printhex_diff(taintmem_backup, 32, taintmem_test, 32, 1);
		e=err = EXIT_FAILURE;
	}
	if ( (bcmp(fx_backup, fx_test, 240)) || /* exempt scratch register */
	     (bcmp(&fx_backup[256], &fx_test[256], 256)) )
	{
		print_fxsave(fx_backup, fx_test);
		e=err = EXIT_FAILURE;
	}
	if (e)
		printhex(opcode, oplen);
}

void ref_copy_reg32_to_reg32(int from_reg, int to_reg)
{
	taint_regs[to_reg] = taint_regs[from_reg];
}

void ref_copy_reg16_to_reg16(int from_reg, int to_reg)
{
	taint_regs[to_reg] = (taint_regs[to_reg]&0xFFFF0000) | (taint_regs[from_reg]&0xFFFF);
}

char *get_byte_reg(int reg)
{
	int m[] = { 0, 4, 8, 12, 1, 5, 9, 13 };
	return &((char*)taint_regs)[m[reg]];
}

void ref_copy_reg8_to_reg8(int from_reg, int to_reg)
{
	char *f = get_byte_reg(from_reg),
	     *t = get_byte_reg(to_reg);

	*t = *f;
}

void ref_or_reg32_to_reg32(int from_reg, int to_reg)
{
	taint_regs[to_reg] |= taint_regs[from_reg];
}

void ref_or_reg16_to_reg16(int from_reg, int to_reg)
{
	taint_regs[to_reg] |= taint_regs[from_reg]&0xFFFF;
}

void ref_erase_reg32(int reg)
{
	taint_regs[reg] = 0;
}

void ref_erase_reg16(int reg)
{
	taint_regs[reg] &= 0xFFFF0000;
}

void ref_erase_reg8(int reg)
{
	*get_byte_reg(reg) = 0;
}

void ref_erase_hireg16(int reg)
{
	taint_regs[reg] &= 0xFFFF;
}

void ref_erase_mem32(char *mrm, long off)
{
	if (!is_memop(mrm)) { ref_erase_reg32(mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(long *)(addr+off) = 0;
}

void ref_erase_mem16(char *mrm, long off)
{
	if (!is_memop(mrm)) { ref_erase_reg16(mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(long *)(addr+off) &= 0xFFFF0000;
}

void ref_erase_mem8(char *mrm, long off)
{
	if (!is_memop(mrm)) { ref_erase_reg8(mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(char *)(addr+off) = 0;
}

void ref_erase_push32(long off)
{
	*(long *)(regs_test[4]-4+off) = 0;;
}

void ref_erase_push16(long off)
{
	*(short *)(regs_test[4]-2+off) = 0;;
}

void ref_copy_mem32_to_reg32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg32_to_reg32(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] = *(long *)(addr+off);
}

void ref_copy_mem16_to_reg16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg16_to_reg16(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] = (taint_regs[reg]&0xFFFF0000) | (*(long *)(addr+off)&0xFFFF);
}

void ref_copy_mem8_to_reg8(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg8_to_reg8(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);
	char *t = get_byte_reg(reg);

	*t = *(char *)(addr+off);
}

void ref_copy_reg32_to_mem32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg32_to_reg32(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(long *)(addr+off) = taint_regs[reg];
}

void ref_copy_reg16_to_mem16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg16_to_reg16(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(long *)(addr+off) = (*(long *)(addr+off)&0xFFFF0000) | (taint_regs[reg]&0xFFFF);
}

void ref_copy_reg8_to_mem8(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg8_to_reg8(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);
	char *f = get_byte_reg(reg);

	*(char *)(addr+off) = *f;
}

void ref_copy_push_reg32(int reg, long off)
{
	*(long *)(regs_test[4]+off-4) = taint_regs[reg];
}

void ref_copy_push_reg16(int reg, long off)
{
	*(short *)(regs_test[4]+off-2) = taint_regs[reg];
}

void ref_copy_pop_reg32(int reg, long off)
{
	taint_regs[reg] = *(long *)(regs_test[4]+off);
}

void ref_copy_pop_reg16(int reg, long off)
{
	taint_regs[reg] = (taint_regs[reg]&0xFFFF0000) | *(unsigned short *)(regs_test[4]+off);
}

void ref_copy_push_mem32(char *mrm, long off)
{
	char *addr = do_lea(mrm);
	*(long *)(regs_test[4]+off-4) = *(long *)(addr+off);
}

void ref_copy_push_mem16(char *mrm, long off)
{
	char *addr = do_lea(mrm);

	*(short *)(regs_test[4]+off-2) = *(short *)(addr+off);
}

void ref_copy_pop_mem32(char *mrm, long off)
{
	char *addr = do_lea(mrm);

	*(long *)(addr+off) = *(long *)(regs_test[4]+off);
}

void ref_copy_pop_mem16(char *mrm, long off)
{
	char *addr = do_lea(mrm);

	*(short *)(addr+off) = *(short *)(regs_test[4]+off);
}

void ref_copy_eax_to_addr32(long addr, long off)
{
	*(long*)(addr+off) = taint_regs[0];
}

void ref_copy_ax_to_addr16(long addr, long off)
{
	*(short*)(addr+off) = taint_regs[0];
}

void ref_copy_al_to_addr8(long addr, long off)
{
	*(char*)(addr+off) = taint_regs[0];
}

void ref_copy_addr32_to_eax(long addr, long off)
{
	taint_regs[0] = *(long*)(addr+off);
}

void ref_copy_addr16_to_ax(long addr, long off)
{
	taint_regs[0] = (taint_regs[0]&0xFFFF0000) | *(unsigned short*)(addr+off);
}

void ref_copy_addr8_to_al(long addr, long off)
{
	taint_regs[0] = (taint_regs[0]&0xFFFFFF00) | *(unsigned char*)(addr+off);
}

void ref_copy_eax_to_str32(long off)
{
	*(long*)(off+regs_test[7]) = taint_regs[0];
}

void ref_copy_ax_to_str16(long off)
{
	*(short*)(off+regs_test[7]) = (short)taint_regs[0];
}

void ref_copy_al_to_str8(long off)
{
	*(char*)(off+regs_test[7]) = (char)taint_regs[0];
}

void ref_copy_str32_to_eax(long off)
{
	taint_regs[0] = *(long*)(off+regs_test[6]);
}

void ref_copy_str16_to_ax(long off)
{
	taint_regs[0] = (taint_regs[0]&0xFFFF0000) | *(unsigned short*)(off+regs_test[6]);
}

void ref_copy_str8_to_al(long off)
{
	taint_regs[0] = (taint_regs[0]&0xFFFFFF00) | *(unsigned char*)(off+regs_test[6]);
}

void ref_copy_str32_to_str32(long off)
{
	*(long*)(off+regs_test[7]) = *(long*)(off+regs_test[6]);
}

void ref_copy_str16_to_str16(long off)
{
	*(short*)(off+regs_test[7]) = *(short*)(off+regs_test[6]);
}

void ref_copy_str8_to_str8(long off)
{
	*(char*)(off+regs_test[7]) = *(char*)(off+regs_test[6]);
}

void ref_or_mem32_to_reg32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_or_reg32_to_reg32(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] |= *(long *)(addr+off);
}

void ref_or_mem16_to_reg16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_or_reg16_to_reg16(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] |= *(unsigned short *)(addr+off);
}

void ref_xor_mem32_to_reg32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm) && reg == (mrm[0]&0x7)) { ref_erase_reg32(reg); return; }
	ref_or_mem32_to_reg32(mrm, off);
}

void ref_xor_mem16_to_reg16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm) && reg == (mrm[0]&0x7)) { ref_erase_reg16(reg); return; }
	ref_or_mem16_to_reg16(mrm, off);
}

void ref_or_reg32_to_mem32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_or_reg32_to_reg32(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(long *)(addr+off) |= taint_regs[reg];
}

void ref_or_reg16_to_mem16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_or_reg16_to_reg16(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	*(short *)(addr+off) |= taint_regs[reg];
}

void ref_xor_reg32_to_mem32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm) && reg == (mrm[0]&0x7)) { ref_erase_reg32(reg); return; }
	ref_or_reg32_to_mem32(mrm, off);
}

void ref_xor_reg16_to_mem16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm) && reg == (mrm[0]&0x7)) { ref_erase_reg16(reg); return; }
	ref_or_reg16_to_mem16(mrm, off);
}


void ref_swap_reg32_reg32(int reg1, int reg2)
{
	long tmp = taint_regs[reg1];
	taint_regs[reg1] = taint_regs[reg2];
	taint_regs[reg2] = tmp;
}

void ref_swap_reg16_reg16(int reg1, int reg2)
{
	unsigned short tmp = (unsigned short)taint_regs[reg1];
	taint_regs[reg1] = (taint_regs[reg1]&0xFFFF0000) | (taint_regs[reg2]&0xFFFF);
	taint_regs[reg2] = (taint_regs[reg2]&0xFFFF0000) | tmp;
}

void ref_swap_reg8_reg8(int reg1, int reg2)
{
	char *r1 = get_byte_reg(reg1);
	char *r2 = get_byte_reg(reg2);
	char tmp = *r1; *r1=*r2; *r2=tmp;
}

void ref_swap_reg32_mem32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_swap_reg32_reg32(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	long tmp = taint_regs[reg];
	taint_regs[reg] = *(long *)(addr+off);
	*(long *)(addr+off) = tmp;
}

void ref_swap_reg16_mem16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_swap_reg16_reg16(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	unsigned short tmp = *(unsigned short*)(addr+off);
	*(unsigned short*)(addr+off) = taint_regs[reg]&0xFFFF;
	taint_regs[reg] = (taint_regs[reg]&0xFFFF0000) | tmp;
}

void ref_swap_reg8_mem8(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_swap_reg8_reg8(reg, mrm[0]&0x7); return; }
	char *addr = do_lea(mrm);

	char *r = get_byte_reg(reg);

	char tmp = *(char*)(addr+off);
	*(char*)(addr+off) = *r;
	*r = tmp;
}

void ref_copy_reg16_to_reg32(int from_reg, int to_reg)
{
	taint_regs[to_reg] = taint_regs[from_reg]&0xFFFF;
}

void ref_copy_reg8_to_reg32(int from_reg, int to_reg)
{
	taint_regs[to_reg] = taint_regs[from_reg]&0xFF;
}

void ref_copy_reg8_to_reg16(int from_reg, int to_reg)
{
	char *f=get_byte_reg(from_reg);
	taint_regs[to_reg] = (taint_regs[to_reg]&0xFFFF0000) | (unsigned char)(*f);
}

void ref_copy_mem16_to_reg32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg16_to_reg32(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] = *(unsigned short*)(addr+off);
}

void ref_copy_mem8_to_reg32(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg8_to_reg32(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] = *(unsigned char*)(addr+off);
}

void ref_copy_mem8_to_reg16(char *mrm, long off)
{
	int reg = (mrm[0]>>3)&0x7;
	if (!is_memop(mrm)) { ref_copy_reg8_to_reg16(mrm[0]&0x7, reg); return; }
	char *addr = do_lea(mrm);

	taint_regs[reg] = (taint_regs[reg]&0xFFFF0000) | *(unsigned char*)(addr+off);
}

void test_mem(int (*taint_op)(char *, char *, long), void (*ref_op)(char *, long))
{
	int i, j;
	char mrm[16];

	for (i=0; i<8; i++)
		for (j=0; mrm_tests[j]; j++)
		{
			memcpy(mrm, mrm_tests[j], mrm_len(mrm_tests[j]));
			mrm[0] |= i<<3;
			setup();
			ref_op(mrm, offset);
			backup();
			load_fx(fx_test);
			oplen = taint_op(opcode, mrm, offset);
			codeexec((char *)opcode, oplen, (long *)regs_test);
			save_fx(fx_test);
			diff();
		}
}

void test_memonly(int (*taint_op)(char *, char *, long), void (*ref_op)(char *, long))
{
	int i, j;
	char mrm[16];

	for (i=0; i<8; i++)
		for (j=0; mrm_tests[j]; j++)
		{
			memcpy(mrm, mrm_tests[j], mrm_len(mrm_tests[j]));
			mrm[0] |= i<<3;
			if (!is_memop(mrm))
				continue;
			setup();
			ref_op(mrm, offset);
			backup();
			load_fx(fx_test);
			oplen = taint_op(opcode, mrm, offset);
			codeexec((char *)opcode, oplen, (long *)regs_test);
			save_fx(fx_test);
			diff();
		}
}

void test_reg(int (*taint_op)(char *, int), void (*ref_op)(int))
{
	int i;

	for (i=0; i<8; i++)
	{
		setup();
		ref_op(i);
		backup();
		load_fx(fx_test);
		oplen = taint_op(opcode, i);
		codeexec((char *)opcode, oplen, (long *)regs_test);
		save_fx(fx_test);
		diff();
	}
}

void test_stackop(int (*taint_op)(char *, int, long), void (*ref_op)(int, long))
{
	int i;

	for (i=0; i<8; i++)
	{
		setup();
		ref_op(i, offset);
		backup();
		load_fx(fx_test);
		oplen = taint_op(opcode, i, offset);
		codeexec((char *)opcode, oplen, (long *)regs_test);
		save_fx(fx_test);
		diff();
	}
}

void test_addr(int (*taint_op)(char *, long, long), void (*ref_op)(long, long))
{
	int i;

	for (i=0; i<8; i++)
	{
		setup();
		ref_op(regs_test[i], offset);
		backup();
		load_fx(fx_test);
		oplen = taint_op(opcode, regs_test[i], offset);
		codeexec((char *)opcode, oplen, (long *)regs_test);
		save_fx(fx_test);
		diff();
	}
}

void test_impl(int (*taint_op)(char *, long), void (*ref_op)(long))
{
	setup();
	ref_op(offset);
	backup();
	load_fx(fx_test);
	oplen = taint_op(opcode, offset);
	codeexec((char *)opcode, oplen, (long *)regs_test);
	save_fx(fx_test);
	diff();
}

void test_reg2(int (*taint_op)(char *, int, int), void (*ref_op)(int, int))
{
	int i, j;

	for (i=0; i<8; i++)
		for (j=0; j<8; j++)
		{
			setup();
			ref_op(i, j);
			backup();
			load_fx(fx_test);
			oplen = taint_op(opcode, i, j);
			codeexec((char *)opcode, oplen, (long *)regs_test);
			save_fx(fx_test);
			diff();
		}
}

int main(int argc, char **argv)
{
	debug_init(stdout);
	save_fx(fx_orig);
	memcpy(&fx_orig[240], scratch_orig, 4*sizeof(long));
	memcpy(&fx_orig[256], taint_orig, 8*sizeof(long));
	offset = (long)taintmem_test - (long)mem_test;
	codeexec(NULL, 0, (long *)regs_orig);

	test_reg2(taint_copy_reg32_to_reg32, ref_copy_reg32_to_reg32);
	test_reg2(taint_copy_reg16_to_reg16, ref_copy_reg16_to_reg16);
	test_reg2(taint_copy_reg8_to_reg8, ref_copy_reg8_to_reg8);

	test_mem(taint_copy_mem32_to_reg32, ref_copy_mem32_to_reg32);
	test_mem(taint_copy_mem16_to_reg16, ref_copy_mem16_to_reg16);
	test_mem(taint_copy_mem8_to_reg8, ref_copy_mem8_to_reg8);

	test_mem(taint_copy_reg32_to_mem32, ref_copy_reg32_to_mem32);
	test_mem(taint_copy_reg16_to_mem16, ref_copy_reg16_to_mem16);
	test_mem(taint_copy_reg8_to_mem8, ref_copy_reg8_to_mem8);

	test_stackop(taint_copy_push_reg32, ref_copy_push_reg32);
	test_stackop(taint_copy_push_reg16, ref_copy_push_reg16);

	test_memonly(taint_copy_push_mem32, ref_copy_push_mem32);
	test_memonly(taint_copy_push_mem16, ref_copy_push_mem16);

	test_stackop(taint_copy_pop_reg32, ref_copy_pop_reg32);
	test_stackop(taint_copy_pop_reg16, ref_copy_pop_reg16);

	test_memonly(taint_copy_pop_mem32, ref_copy_pop_mem32);
	test_memonly(taint_copy_pop_mem16, ref_copy_pop_mem16);

	test_addr(taint_copy_eax_to_addr32, ref_copy_eax_to_addr32);
	test_addr(taint_copy_ax_to_addr16, ref_copy_ax_to_addr16);
	test_addr(taint_copy_al_to_addr8, ref_copy_al_to_addr8);

	test_addr(taint_copy_addr32_to_eax, ref_copy_addr32_to_eax);
	test_addr(taint_copy_addr16_to_ax, ref_copy_addr16_to_ax);
	test_addr(taint_copy_addr8_to_al, ref_copy_addr8_to_al);

	test_impl(taint_copy_eax_to_str32, ref_copy_eax_to_str32);
	test_impl(taint_copy_ax_to_str16, ref_copy_ax_to_str16);
	test_impl(taint_copy_al_to_str8, ref_copy_al_to_str8);

	test_impl(taint_copy_str32_to_eax, ref_copy_str32_to_eax);
	test_impl(taint_copy_str16_to_ax, ref_copy_str16_to_ax);
	test_impl(taint_copy_str8_to_al, ref_copy_str8_to_al);

	test_impl(taint_copy_str32_to_str32, ref_copy_str32_to_str32);
	test_impl(taint_copy_str16_to_str16, ref_copy_str16_to_str16);
	test_impl(taint_copy_str8_to_str8, ref_copy_str8_to_str8);

	test_reg(taint_erase_reg32, ref_erase_reg32);
	test_reg(taint_erase_reg16, ref_erase_reg16);
	test_reg(taint_erase_reg8, ref_erase_reg8);

	test_mem(taint_erase_mem32, ref_erase_mem32);
	test_mem(taint_erase_mem16, ref_erase_mem16);
	test_mem(taint_erase_mem8, ref_erase_mem8);

	test_reg(taint_erase_hireg16, ref_erase_hireg16);

	test_impl(taint_erase_push32, ref_erase_push32);
	test_impl(taint_erase_push16, ref_erase_push16);

	test_reg2(taint_or_reg32_to_reg32, ref_or_reg32_to_reg32);
	test_reg2(taint_or_reg16_to_reg16, ref_or_reg16_to_reg16);

	test_mem(taint_or_reg32_to_mem32, ref_or_reg32_to_mem32);
	test_mem(taint_or_reg16_to_mem16, ref_or_reg16_to_mem16);

	test_mem(taint_or_mem32_to_reg32, ref_or_mem32_to_reg32);
	test_mem(taint_or_mem16_to_reg16, ref_or_mem16_to_reg16);

	test_mem(taint_xor_reg32_to_mem32, ref_xor_reg32_to_mem32);
	test_mem(taint_xor_reg16_to_mem16, ref_xor_reg16_to_mem16);

	test_mem(taint_xor_mem32_to_reg32, ref_xor_mem32_to_reg32);
	test_mem(taint_xor_mem16_to_reg16, ref_xor_mem16_to_reg16);

	test_reg2(taint_swap_reg32_reg32, ref_swap_reg32_reg32);
	test_reg2(taint_swap_reg16_reg16, ref_swap_reg16_reg16);
	test_reg2(taint_swap_reg8_reg8, ref_swap_reg8_reg8);

	test_mem(taint_swap_reg32_mem32, ref_swap_reg32_mem32);
	test_mem(taint_swap_reg16_mem16, ref_swap_reg16_mem16);
	test_mem(taint_swap_reg8_mem8, ref_swap_reg8_mem8);

	test_reg2(taint_copy_reg16_to_reg32, ref_copy_reg16_to_reg32);
	test_reg2(taint_copy_reg8_to_reg32, ref_copy_reg8_to_reg32);
	test_reg2(taint_copy_reg8_to_reg16, ref_copy_reg8_to_reg16);

	test_mem(taint_copy_mem16_to_reg32, ref_copy_mem16_to_reg32);
	test_mem(taint_copy_mem8_to_reg32, ref_copy_mem8_to_reg32);
	test_mem(taint_copy_mem8_to_reg16, ref_copy_mem8_to_reg16);

	exit(err);
}
