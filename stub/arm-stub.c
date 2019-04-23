/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 by Tomasz Malesinski
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <xinu.h>
#include <stdbool.h>
#include <string.h>

#define BUFMAX 1024

#define VEC_UND 1
#define VEC_SWI 2
#define VEC_PABT 3
#define VEC_DABT 4


#define STUB_STACK_SIZE 8192
char _stub_stack[STUB_STACK_SIZE];

static char packet_buf[BUFMAX];
static char reply_buf[BUFMAX];

static const char hexchars[] = "0123456789abcdef";
int gdb_exception_no, gdb_mem_access;
static unsigned long registers[17];

void breakpoint(void);
//static void gdb_api_log(char *msg);

/*
__attribute__((section(".gdbapi"))) struct gdb_api gdb_api =
{
    GDB_API_MAGIC,
    {breakpoint, gdb_api_log}
};
*/


static inline bool isxdigit(char c)
{
    return ((c >= '0') && (c <= '9'))
        || ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'));
}

static int hex(char ch) {
    if ((ch >= 'a') && (ch <= 'f'))
        return ch - 'a' + 10;
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if ((ch >= 'A') && (ch <= 'F'))
        return ch - 'A' + 10;
    return -1;
}

int hexToInt(char **ptr, int *intValue) {
    int numChars = 0;
    int hexValue;

    *intValue = 0;
    while (**ptr) {
        hexValue = hex(**ptr);
        if (hexValue >= 0) {
            *intValue = (*intValue << 4) | hexValue;
            numChars++;
        } else {
            break;
        }

        (*ptr)++;
    }

    return numChars;
}

static void hex_byte(char *s, int byte) {
    s[0] = hexchars[(byte >> 4) & 0xf];
    s[1] = hexchars[byte & 0xf];
}

static void hex_word(char *s, unsigned long val) {
    int i;
    for (i = 0; i < 4; i++)
        hex_byte(s + i * 2, (val >> (i * 8)) & 0xff);
}

static void hex_string(char *d, char *s) {
    while (*s) {
        hex_byte(d, *s++);
        d += 2;
    }
    *d = 0;
}

static int get_hex_byte(char *s) {
    return (hex(s[0]) << 4) + hex(s[1]);
}

static unsigned long get_hex_word(char *s) {
    int i;
    unsigned long r = 0;
    for (i = 3; i >= 0; i--)
        r = (r << 8) + get_hex_byte(s + i * 2);
    return r;
}

static void reply_error(int n, char *reply) {
    reply[0] = 'E';
    hex_byte(reply + 1, n);
    reply[3] = 0;
}

static void reply_signal(int n, char *reply) {
    int signal;
    reply[0] = 'S';
    switch (n)
    {
        case VEC_UND:
            signal = 4;
            break;
        case VEC_PABT:
        case VEC_DABT:
            signal = 7;
            break;
        default:
            signal = 5;
            break;
    }
    hex_byte(reply + 1, signal);
    reply[3] = 0;
}

static void reply_ok(char *reply) {
    strcpy(reply, "OK");
}


static int get_byte(void) {
    struct dentry* devptr;
    struct uart_csreg* regptr;
    int irmask;

    int c;
    devptr = (struct dentry *) &devtab[CONSOLE];
    regptr = (struct uart_csreg *)devptr->dvcsr;

    irmask = regptr->ier;       /* Save UART interrupt state.   */
    regptr->ier = 0;        /* Disable UART interrupts.     */

    while (0 == (regptr->lsr & UART_LSR_DR)) {
        ; /* Do Nothing */
    }

    /* Read character from Receive Holding Register */

    c = regptr->rbr;
    regptr->ier = irmask;       /* Restore UART interrupts.     */
    return c;
}

static void put_byte(unsigned char c) {
    struct uart_csreg* csrptr;
    struct dentry* devptr;

    /* Get CSR address of the console */
    devptr = (struct dentry *) &devtab[CONSOLE];
    csrptr = (struct uart_csreg *) devptr->dvcsr;

    /* wait for UART transmit queue to empty */
    while ( (csrptr->lsr & UART_LSR_THRE) == 0 ) {
        ;
    }

    /* write the character */
    csrptr->buffer = c;
}

void send_gdb_header(){
    put_byte('\02'); //STX
    put_byte('G');
}

static void serial_write(unsigned char *buf, int len) {
    int i;
    for (i = 0; i < len; i++)
        put_byte(buf[i]);
}

static void get_packet(char *buf, int len) {
    int count, checksum, escaped;
    int ch;

    while (1) {
        do {
            ch = get_byte();
        } while (ch != '$');

        checksum = 0;
        count = 0;
        escaped = 0;
        while (count < len) {
            ch = get_byte();
            if (!escaped) {
                if (ch == '$') {
                    checksum = 0;
                    count = 0;
                } else if (ch == '#')
                    break;
                else if (ch == 0x7d) {
                    escaped = 1;
                    checksum += ch;
                } else {
                    checksum += ch;
                    buf[count] = ch;
                    count++;
                }
            } else {
                escaped = 0;
                checksum += ch;
                buf[count] = ch ^ 0x20;
                count++;
            }
        }
        buf[count] = 0;

        if (ch == '#') {
            int rchksum;

            ch = get_byte();
            rchksum = hex(ch) << 4;
            ch = get_byte();
            rchksum += hex(ch);
            
            send_gdb_header();
            if ((checksum & 0xff) != rchksum)
                put_byte('-');
            else {
                put_byte('+');
                put_byte('\04');
                return;
            }
            put_byte('\04');
        }
    }
}

static void put_packet(char *buf) {
    int i, checksum;
    int ch;
    char tmp[3];

    do {
        send_gdb_header();
        put_byte('$');

        checksum = 0;
        for (i = 0; buf[i]; i++)
            checksum += buf[i];

        serial_write(buf, i);

        tmp[0] = '#';
        hex_byte(tmp + 1, checksum & 0xff);
        serial_write(tmp, 3);
        
        put_byte('\04'); //EOT        

        ch = get_byte();

    } while (ch != '+');
}

static inline unsigned long get_general_reg(int n)
{
    return registers[n + 1];
}

static inline void set_general_reg(int n, unsigned long v)
{
    registers[n + 1] = v;
}

static inline unsigned long get_cpsr(void)
{
    return registers[0];
}

static inline void set_cpsr(unsigned long v)
{
    registers[0] = v;
}

static void g_reply(char *buf) {
    int i;
    char *p;

    p = buf;
    for (i = 0; i < 16; i++) {
        hex_word(p, get_general_reg(i));
        p += 8;
    }

    for (i = 0; i < 8; i++) {
        memset(p, '0', 16);
        p += 16;
    }

    hex_word(p, 0);
    p += 8;
    hex_word(p, get_cpsr());
    p[8] = 0;
}

static void cmd_get_register(char *args, char *reply) {
    int r;

    if (!hexToInt(&args, &r)) {
        reply_error(0, reply);
        return;
    }

    if (r >= 0 && r < 16) {
        hex_word(reply, get_general_reg(r));
        reply[8] = 0;
    } else if (r == 25) {
        hex_word(reply, get_cpsr());
        reply[8] = 0;
    } else {
        hex_word(reply, 0);
        reply[8] = 0;
    }
    return;
}

static void cmd_set_register(char *args, char *reply) {
  int r, p;
  unsigned long v;

  if (!hexToInt(&args, &r)) {
    goto error;
  }
  if (*(args++) != '=') {
    goto error;
  }
  if (!hexToInt(&args, &p)) {
    goto error;
  }

  v = get_hex_word(args + p);
  if (r >= 0 && r < 16)
    set_general_reg(r, v);
  else if (r == 25)
    set_cpsr(v);
  reply_ok(reply);
  return;

error:
  reply_error(0, reply);
  return;
}

static void cmd_set_registers(char *args, char *reply) {
    char *p;
    int i, len;

    len = strlen(args);

    p = args;
    for (i = 0; i < 16 && len >= (i + 1) * 8; i++) {
        set_general_reg(i, get_hex_word(p));
        p += 8;
    }

    if (len >= 16 * 8 + 8 * 16 + 2 * 8)
    {
        p += 8 * 16 + 8;
        set_cpsr(get_hex_word(p));
    }

    reply_ok(reply);
}

static void cmd_get_memory(char *args, char *reply) {
    unsigned long addr, len, i;

    if (!hexToInt(&args, &addr)) {
        goto error;
    }
    if (*(args++) != ',') {
        goto error;
    }
    if (!hexToInt(&args, &len)) {
        goto error;
    }

    if (len > (BUFMAX - 16) / 2) {
        reply_error(1, reply);
        return;
    }
  
    gdb_mem_access = 1;
    for (i = 0; i < len; i++)
        hex_byte(reply + i * 2, *((unsigned char *)(addr + i)));
    gdb_mem_access = 0;

    reply[len * 2] = 0;
    return;
error:
    reply_error(0, reply);
    return;
}

static void cmd_put_memory(char *args, char *reply) {
    unsigned long addr, len, i;
    int pos;
    char* original = args;

    if (!hexToInt(&args, &addr)) {
        goto error;
    }
    if (*(args++) != ',') {
        goto error;
    }
    if (!hexToInt(&args, &len)) {
        goto error;
    }

    pos = (int) (args - original);

    gdb_mem_access = 1;
    for (i = 0; i < len; i++)
        *((unsigned char *)(addr + i)) = get_hex_byte(args + pos + i * 2);
    gdb_mem_access = 0;

    reply_ok(reply);
    return;
error:
    reply_error(0, reply);
    return;
}

static void cmd_put_memory_binary(char *args, char *reply) {
    unsigned long addr, len, i;
    int pos;
    char* original = args;

    if (!hexToInt(&args, &addr)) {
        goto error;
    }
    if (*(args++) != ',') {
        goto error;
    }
    if (!hexToInt(&args, &len)) {
        goto error;
    }

    pos = (int) (args - original);

    gdb_mem_access = 1;
    for (i = 0; i < len; i++)
        *((unsigned char *)(addr + i)) = args[pos + i];
    gdb_mem_access = 0;

    reply_ok(reply);
    return;
error:
    reply_error(0, reply);
    return;
}

static void parse_continue_args(char *args) {
    int sig;
    unsigned long addr;

    if (hexToInt(&args, &addr)) {
        if (*(args++) == ';') {
            if (hexToInt(&args, &addr)) {
                set_general_reg(15, addr);
            }
        } else {
            set_general_reg(15, addr);
        }
    }
}

static void cmd_go(char *args) {
    parse_continue_args(args);

    asm volatile(
        "    mov    r1, %0\n"
        "    ldr    r12, [r1], #4\n"
        "    mov    r0, r12\n"
        "    and    r0, r0, #0x1f\n"
        "    cmp    r0, #0x10\n"
        "    bne    1f\n"
        "    ldr    r14, [r1, #60]\n"
        "    msr    spsr_fsxc, r12\n"
        "    ldmia  r1, {r0-r14}^\n"
        "    movs   r15, r14\n"
        "1:\n"
        "    msr    cpsr_fsxc, r12\n"
        "    ldmia  r1, {r0-r15}\n"
        : : "r" (registers));
}

void gdb_loop(void) {
    int no_reply;

    gdb_mem_access = 0;

    while (1) {
        get_packet(packet_buf, sizeof(packet_buf) - 1);
            
        no_reply = 0;
        switch (packet_buf[0]) {
            case '?':
                reply_signal(gdb_exception_no, reply_buf);
                break;

            case 'p':
                cmd_get_register(packet_buf + 1, reply_buf);
                break;

            case 'P':
                cmd_set_register(packet_buf + 1, reply_buf);
                break;

            case 'g':
                g_reply(reply_buf);
                break;

            case 'G':
                cmd_set_registers(packet_buf + 1, reply_buf);
                break;

            case 'm':
                cmd_get_memory(packet_buf + 1, reply_buf);
                break;

            case 'M':
                cmd_put_memory(packet_buf + 1, reply_buf);
                break;

            case 'X':
                cmd_put_memory_binary(packet_buf + 1, reply_buf);
                break;

            case 'q':
                //cmd_query(packet_buf + 1, reply_buf);
                break;

            case 'c':
                cmd_go(packet_buf + 1);
                reply_error(1, reply_buf);
                break;

/*             case 's': */
/*                 cmd_go(packet_buf + 1); */
/*                 break; */

            default:
                reply_buf[0] = 0;
        }

        if (!no_reply)
            put_packet(reply_buf);
    }
}

void set_debug_traps(int n, void *p)
{
    set_evec(n, (uint32)p);
}

void gdb_und_exc(void);
void gdb_swi_exc(void);
void gdb_pabt_exc(void);
void gdb_dabt_exc(void);

static void gdb_set_vectors(void)
{
    gdb_set_vector(VEC_UND, gdb_und_exc);
    gdb_set_vector(VEC_SWI, gdb_swi_exc);
    gdb_set_vector(VEC_PABT, gdb_pabt_exc);
    gdb_set_vector(VEC_DABT, gdb_dabt_exc);
}

void gdb_loop_from_exc(void)
{
    if (gdb_mem_access)
        reply_error(1, reply_buf);
    else        
        reply_signal(gdb_exception_no, reply_buf);
    put_packet(reply_buf);
    gdb_loop();
}

#define IRQ_REG(reg) (*(volatile unsigned long *)(0x80300000 + (reg)))

static inline unsigned long irq_read(int reg)
{
    unsigned long v, v2;
    do
    {
        v = IRQ_REG(reg);
        v2 = IRQ_REG(reg);
    } while (v != v2);
    return v;
}

#define IRQ_WRITE_WAIT(reg, val, cond) \
    do { unsigned long v, v2; \
        do { \
            IRQ_REG(reg) = (val); \
            v = IRQ_REG(reg); \
            v2 = IRQ_REG(reg); \
        } while ((v != v2) || !(cond)); \
    } while (0);

void fiq(void)
{
}


#define str(s) #s
#define xstr(s) str(s)

asm (".text\n"
     "gdb_und_exc:\n"
     "    ldr    sp, =_stub_stack\n"
     "    sub    r14, r14, #4\n"
     "    stmfd  sp!, {r0-r3, r12, r14}\n"
     "    mov    r0, #" xstr(VEC_UND) "\n"
     "    b      gdb_handle_exception\n"
     "gdb_swi_exc:\n"
     "    ldr    sp, =_stub_stack\n"
     "    stmfd  sp!, {r0-r3, r12, r14}\n"
     "    mov    r0, #" xstr(VEC_SWI) "\n"
     "    b      gdb_handle_exception\n"
     "gdb_pabt_exc:\n"
     "    ldr    sp, =_stub_stack\n"
     "    stmfd  sp!, {r0-r3, r12, r14}\n"
     "    mov    r0, #" xstr(VEC_PABT) "\n"
     "    b      gdb_handle_exception\n"
     "gdb_dabt_exc:\n"
     "    ldr    sp, =_stub_stack\n"
     "    sub    r14, r14, #4\n"
     "    stmfd  sp!, {r0-r3, r12, r14}\n"
     "    ldr    r0, =gdb_mem_access\n"
     "    ldr    r0, [r0]\n"
     "    tst    r0, r0\n"
     "    bne    gdb_data_abort\n"
     "    mov    r0, #" xstr(VEC_DABT) "\n"
     "    b      gdb_handle_exception\n"
     "gdb_handle_exception:\n"
     "    ldr    r1, =gdb_exception_no\n"
     "    str    r0, [r1]\n"
     "    ldr    r0, =registers\n"
     "    mrs    r12, spsr\n"
     "    str    r12, [r0], #4\n"
     "    ldmfd  sp!, {r2, r3}\n"
     "    stmia  r0!, {r2, r3}\n"
     "    ldmfd  sp!, {r2, r3, r12, r14}\n"
     "    str    r14, [r0, #52]\n"
     "    stmia  r0!, {r2-r12}\n"
     "    mrs    r1, spsr\n"
     "    and    r2, r1, #0x1f\n"
     "    cmp    r2, #0x10\n"
     "    bne    1f\n"
     "    stmia  r0, {r13, r14}^\n"
     "    b      gdb_data_abort\n"
     "1:\n"
     "    msr    cpsr_c, r1\n"
     "    stmia  r0, {r13, r14}\n"
     "gdb_data_abort:\n"
     "    msr    cpsr_c, #0xd3\n"
     "    ldr    sp, =_stub_stack\n"
     "    b      gdb_loop_from_exc\n"
     ".globl breakpoint\n"
     "breakpoint:\n"
     "    stmfd  sp!, {r0-r1}\n"
     "    ldr    r0, =registers\n"
     "    mrs    r1, cpsr\n"
     "    str    r1, [r0], #4\n"
     "    ldmfd  sp!, {r1}\n"
     "    str    r1, [r0], #4\n"
     "    ldmfd  sp!, {r1}\n"
     "    str    r1, [r0], #4\n"
     "    stmia  r0!, {r2-r14}\n"
     "    str    r14, [r0]\n"
     "    msr    cpsr_c, #0xd3\n"
     "    ldr    sp, =_stub_stack\n"
     "    ldr    r0, =gdb_exception_no\n"
     "    mov    r1, #5\n"
     "    str    r1, [r0]\n"
     "    b      gdb_loop_from_exc\n");
