// CPU emulator is a modified version of semu, written by Jim Huang (jserv)
// https://github.com/jserv/semu

#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keyboard.h"
#include "semu.h"
#include "mul128.h"

/* Range check
 * For any variable range checking:
 *     if (x >= minx && x <= maxx) ...
 * it is faster to use bit operation:
 *     if ((signed)((x - minx) | (maxx - x)) >= 0) ...
 */
#define RANGE_CHECK(x, minx, size) \
    ((int32_t) ((x - minx) | (minx + size - 1 - x)) >= 0)

/* CSR is Control Status Register representation in RISC-V privileged
 * architecture.
 */

/* Machine level CSRs */
enum { MSTATUS = 0x300, MEDELEG = 0x302, MIDELEG, MIE, MTVEC };
enum { MEPC = 0x341, MCAUSE, MTVAL, MIP };

/* Supervisor level CSRs */
enum { SSTATUS = 0x100, SIE = 0x104, STVEC };
enum { SEPC = 0x141, SCAUSE, STVAL, SIP, SATP = 0x180 };

enum { MIP_SSIP = 1ULL << 1, MIP_MSIP = 1ULL << 3, MIP_STIP = 1ULL << 5 };
enum { MIP_MTIP = 1ULL << 7, MIP_SEIP = 1ULL << 9, MIP_MEIP = 1ULL << 11 };

#define PAGE_SIZE 4096 /* should be configurable */

#define MAX(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a > _b ? _a : _b;      \
    })

#define MIN(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a < _b ? _a : _b;      \
    })

/* Check alignement of the address (Assume alignment is a power of 2.)
 * x is the address to be checked.
 * a is the alignment constaint and must be power of 2.
 */
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x)) (a) -1)) == 0)

bool exception_is_fatal(const exception_t e)
{
    switch (e) {
    case INSTRUCTION_ADDRESS_MISALIGNED:
    case INSTRUCTION_ACCESS_FAULT:
    case LOAD_ACCESS_FAULT:
    case STORE_AMO_ADDRESS_MISALIGNED:
    case STORE_AMO_ACCESS_FAULT:
        return true;
    default:
        return false;
    }
}

struct ram *ram_new(const uint8_t *code, const size_t code_size)
{
    struct ram *ram = calloc(1, sizeof(struct ram));
    ram->data = calloc(RAM_SIZE, 1);
    memcpy(ram->data, code, code_size);
    return ram;
}

exception_t ram_load(const struct ram *ram,
                     const uint64_t addr,
                     const uint64_t size,
                     uint64_t *result)
{
    uint64_t index = addr - RAM_BASE, tmp = 0;
    switch (size) {
    case 64:
        tmp |= (uint64_t) (ram->data[index + 7]) << 56;
        tmp |= (uint64_t) (ram->data[index + 6]) << 48;
        tmp |= (uint64_t) (ram->data[index + 5]) << 40;
        tmp |= (uint64_t) (ram->data[index + 4]) << 32;
    case 32:
        tmp |= (uint64_t) (ram->data[index + 3]) << 24;
        tmp |= (uint64_t) (ram->data[index + 2]) << 16;
    case 16:
        tmp |= (uint64_t) (ram->data[index + 1]) << 8;
    case 8:
        tmp |= (uint64_t) (ram->data[index + 0]) << 0;
        *result = tmp;
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

exception_t ram_store(struct ram *ram,
                      const uint64_t addr,
                      const uint64_t size,
                      const uint64_t value)
{
    uint64_t index = addr - RAM_BASE;
    switch (size) {
    case 64:
        ram->data[index + 7] = (value >> 56) & 0xff;
        ram->data[index + 6] = (value >> 48) & 0xff;
        ram->data[index + 5] = (value >> 40) & 0xff;
        ram->data[index + 4] = (value >> 32) & 0xff;
    case 32:
        ram->data[index + 3] = (value >> 24) & 0xff;
        ram->data[index + 2] = (value >> 16) & 0xff;
    case 16:
        ram->data[index + 1] = (value >> 8) & 0xff;
    case 8:
        ram->data[index + 0] = (value >> 0) & 0xff;
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

void fatal(const char *msg)
{
    fprintf(stderr, "ERROR: Failed to %s.\n", msg);
    exit(1);
}

struct clint *clint_new()
{
    return calloc(1, sizeof(struct clint));
}

static inline exception_t clint_load(const struct clint *clint,
                                     const uint64_t addr,
                                     const uint64_t size,
                                     uint64_t *result)
{
    if (size != 64)
        return LOAD_ACCESS_FAULT;

    switch (addr) {
    case CLINT_MTIMECMP:
        *result = clint->mtimecmp;
        break;
    case CLINT_MTIME:
        *result = clint->mtime;
        break;
    default:
        *result = 0;
    }
    return OK;
}

static inline exception_t clint_store(struct clint *clint,
                                      const uint64_t addr,
                                      const uint64_t size,
                                      const uint64_t value)
{
    if (size != 64)
        return STORE_AMO_ACCESS_FAULT;

    switch (addr) {
    case CLINT_MTIMECMP:
        clint->mtimecmp = value;
        break;
    case CLINT_MTIME:
        clint->mtime = value;
        break;
    }
    return OK;
}

struct plic *plic_new()
{
    return calloc(1, sizeof(struct plic));
}

exception_t plic_load(const struct plic *plic,
                      const uint64_t addr,
                      const uint64_t size,
                      uint64_t *result)
{
    if (size != 32)
        return LOAD_ACCESS_FAULT;

    switch (addr) {
    case PLIC_PENDING:
        *result = plic->pending;
        break;
    case PLIC_SENABLE:
        *result = plic->senable;
        break;
    case PLIC_SPRIORITY:
        *result = plic->spriority;
        break;
    case PLIC_SCLAIM:
        *result = plic->sclaim;
        break;
    default:
        *result = 0;
    }
    return OK;
}

exception_t plic_store(struct plic *plic,
                       const uint64_t addr,
                       const uint64_t size,
                       const uint64_t value)
{
    if (size != 32)
        return STORE_AMO_ACCESS_FAULT;

    switch (addr) {
    case PLIC_PENDING:
        plic->pending = value;
        break;
    case PLIC_SENABLE:
        plic->senable = value;
        break;
    case PLIC_SPRIORITY:
        plic->spriority = value;
        break;
    case PLIC_SCLAIM:
        plic->sclaim = value;
        break;
    }
    return OK;
}

static void *uart_thread_func(void *priv)
{
    struct uart *uart = (struct uart *) priv;
    while (1) {
        struct pollfd pfd = {0, POLLIN, 0};
        poll(&pfd, 1, 0);
        if (!(pfd.revents & POLLIN))
            continue;

        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) /* an error or EOF */
            continue;

        pthread_mutex_lock(&uart->lock);
        while ((uart->data[UART_LSR - UART_BASE] & UART_LSR_RX) == 1)
            pthread_cond_wait(&uart->cond, &uart->lock);

        uart->data[0] = c;
        uart->interrupting = true;
        uart->data[UART_LSR - UART_BASE] |= UART_LSR_RX;
        pthread_mutex_unlock(&uart->lock);
    }

    /* should not reach here */
    return NULL;
}

struct uart *uart_new()
{
    struct uart *uart = calloc(1, sizeof(struct uart));
    uart->data[UART_LSR - UART_BASE] |= UART_LSR_TX;
    pthread_mutex_init(&uart->lock, NULL);
    pthread_cond_init(&uart->cond, NULL);

    pthread_create(&uart->tid, NULL, uart_thread_func, (void *) uart);
    return uart;
}

exception_t uart_load(struct uart *uart,
                      const uint64_t addr,
                      const uint64_t size,
                      uint64_t *result)
{
    if (size != 8)
        return LOAD_ACCESS_FAULT;

    pthread_mutex_lock(&uart->lock);
    switch (addr) {
    case UART_RHR:
        pthread_cond_broadcast(&uart->cond);
        uart->data[UART_LSR - UART_BASE] &= ~UART_LSR_RX;
    default:
        *result = uart->data[addr - UART_BASE];
    }
    pthread_mutex_unlock(&uart->lock);
    return OK;
}

exception_t uart_store(struct uart *uart,
                       const uint64_t addr,
                       const uint64_t size,
                       const uint64_t value)
{
    if (size != 8)
        return STORE_AMO_ACCESS_FAULT;

    pthread_mutex_lock(&uart->lock);
    switch (addr) {
    case UART_THR:
        putchar(value & 0xff);
        fflush(stdout);
        break;
    default:
        uart->data[addr - UART_BASE] = value & 0xff;
    }
    pthread_mutex_unlock(&uart->lock);
    return OK;
}

bool uart_is_interrupting(struct uart *uart)
{
    pthread_mutex_lock(&uart->lock);
    bool interrupting = uart->interrupting;
    uart->interrupting = false;
    pthread_mutex_unlock(&uart->lock);
    return interrupting;
}

struct disk *disk_new(uint8_t *disk)
{
    struct disk *vio = calloc(1, sizeof(struct disk));
    vio->disk = disk;
    vio->notify = -1;
    return vio;
}

exception_t disk_load(const struct disk *vio,
                        const uint64_t addr,
                        const uint64_t size,
                        uint64_t *result)
{
    if (size != 32)
        return LOAD_ACCESS_FAULT;

    switch (addr) {
    case DISK_MAGIC:
        *result = 0x666F7864;
        break;
    case DISK_VERSION:
        *result = 0x01;
        break;
    case DISK_NOTIFY:
        *result = vio->notify;
        break;
    case DISK_DIRECTION:
        *result = vio->direction;
        break;
    case DISK_BUFFER_ADDR_HIGH:
        *result = vio->buffer_address_high;
        break;
    case DISK_BUFFER_ADDR_LOW:
        *result = vio->buffer_address_low;
        break;
    case DISK_BUFFER_LEN_HIGH:
        *result = vio->buffer_length_high;
        break;
    case DISK_BUFFER_LEN_LOW:
        *result = vio->buffer_length_low;
        break;
    case DISK_SECTOR:
        *result = vio->sector;
        break;
    case DISK_DONE:
        *result = vio->done;
        break;
    default:
        *result = 0;
    }
    return OK;
}

exception_t disk_store(struct disk *vio,
                         const uint64_t addr,
                         const uint64_t size,
                         const uint64_t value)
{
    if (size != 32)
        return STORE_AMO_ACCESS_FAULT;

    switch (addr) {
    case DISK_NOTIFY:
        vio->notify = value;
        break;
    case DISK_DIRECTION:
        vio->direction = value;
        break;
    case DISK_BUFFER_ADDR_HIGH:
        vio->buffer_address_high = value;
        break;
    case DISK_BUFFER_ADDR_LOW:
        vio->buffer_address_low = value;
        break;
    case DISK_BUFFER_LEN_HIGH:
        vio->buffer_length_high = value;
        break;
    case DISK_BUFFER_LEN_LOW:
        vio->buffer_length_low = value;
        break;
    case DISK_SECTOR:
        vio->sector = value;
        break;
    case DISK_DONE:
        vio->done = value;
        break;
    }
    return OK;
}

static inline bool disk_is_interrupting(struct disk *vio)
{
    if (vio->notify != -1) {
        vio->notify = -1;
        return true;
    }
    return false;
}

static inline uint64_t disk_disk_read(const struct disk *vio, uint64_t addr)
{
    return vio->disk[addr];
}

static inline void disk_disk_write(struct disk *vio,
                                     uint64_t addr,
                                     uint64_t value)
{
    vio->disk[addr] = (uint8_t) value;
}

exception_t kbd_load(const uint64_t addr,
                    const uint64_t size,
                    uint64_t *result)
{
    if (size != 32)
        return LOAD_ACCESS_FAULT;

    switch (addr) {
    case KBD_GET:
        *result = (uint64_t) key_take();
    default:
        *result = 0;
    }
    return OK;
}

struct bus *bus_new(struct ram *ram, struct disk *vio)
{
    struct bus *bus = calloc(1, sizeof(struct bus));
    bus->ram = ram, bus->disk = vio;
    bus->clint = clint_new(), bus->plic = plic_new(), bus->uart = uart_new();
    return bus;
}

exception_t bus_load(const struct bus *bus,
                     const uint64_t addr,
                     const uint64_t size,
                     uint64_t *result)
{
    if (RANGE_CHECK(addr, CLINT_BASE, CLINT_SIZE))
        return clint_load(bus->clint, addr, size, result);
    if (RANGE_CHECK(addr, PLIC_BASE, PLIC_SIZE))
        return plic_load(bus->plic, addr, size, result);
    if (RANGE_CHECK(addr, UART_BASE, UART_SIZE))
        return uart_load(bus->uart, addr, size, result);
    if (RANGE_CHECK(addr, DISK_BASE, DISK_SIZE))
        return disk_load(bus->disk, addr, size, result);
    if (RANGE_CHECK(addr, KBD_BASE, KBD_SIZE))
        return kbd_load(addr, size, result);
    if (RAM_BASE <= addr)
        return ram_load(bus->ram, addr, size, result);

    return LOAD_ACCESS_FAULT;
}

exception_t bus_store(struct bus *bus,
                      const uint64_t addr,
                      const uint64_t size,
                      const uint64_t value)
{
    if (RANGE_CHECK(addr, CLINT_BASE, CLINT_SIZE))
        return clint_store(bus->clint, addr, size, value);
    if (RANGE_CHECK(addr, PLIC_BASE, PLIC_SIZE))
        return plic_store(bus->plic, addr, size, value);
    if (RANGE_CHECK(addr, UART_BASE, UART_SIZE))
        return uart_store(bus->uart, addr, size, value);
    if (RANGE_CHECK(addr, DISK_BASE, DISK_SIZE))
        return disk_store(bus->disk, addr, size, value);
    if (RAM_BASE <= addr)
        return ram_store(bus->ram, addr, size, value);

    return STORE_AMO_ACCESS_FAULT;
}

void bus_disk_access(struct bus *bus)
{
    uint32_t address_high;
    uint32_t address_low;
    uint64_t address;
    if (bus_load(bus, DISK_BUFFER_ADDR_HIGH, 32, &address_high) != OK)
        fatal("read high address");
    if (bus_load(bus, DISK_BUFFER_ADDR_LOW, 32, &address_low) != OK)
        fatal("read low address");
    address = address_high << 32 | address_low;

    uint32_t length_high;
    uint32_t length_low;
    uint64_t length;
    if (bus_load(bus, DISK_BUFFER_LEN_HIGH, 32, &length_high) != OK)
        fatal("read high length");
    if (bus_load(bus, DISK_BUFFER_LEN_LOW, 32, &length_low) != OK)
        fatal("read low length");
    length = length_high << 32 | length_low;

    uint32_t sector;
    if (bus_load(bus, DISK_SECTOR, 32, &sector) != OK)
        fatal("read sector");

    uint32_t direction;
    if (bus_load(bus, DISK_DIRECTION, 32, &direction) != OK)
        fatal("read direction");

    if (direction == 1) {
        /* Read RAM data and write it to a disk directly (DMA). */
        for (uint64_t i = 0; i < length; i++) {
            uint64_t data;
            if (bus_load(bus, address + i, 8, &data) != OK)
                fatal("read from RAM");
            disk_disk_write(bus->disk, sector * 512 + i, data);
        }
    } else {
        /* Read disk data and write it to RAM directly (DMA). */
        for (uint64_t i = 0; i < length; i++) {
            uint64_t data = disk_disk_read(bus->disk, sector * 512 + i);
            if (bus_store(bus, address + i, 8, data) != OK)
                fatal("write to RAM");
        }
    }

    if (bus_store(bus, DISK_DONE, 32, 0) != OK)
        fatal("write done");
}

struct cpu *cpu_new(uint8_t *code, const size_t code_size, uint8_t *disk)
{
    struct cpu *cpu = calloc(1, sizeof(struct cpu));

    /* Initialize the sp(x2) register. */
    cpu->regs[2] = RAM_BASE + RAM_SIZE;

    cpu->bus = bus_new(ram_new(code, code_size), disk_new(disk));
    cpu->pc = RAM_BASE, cpu->mode = MACHINE;

    return cpu;
}

static uint64_t cpu_load_csr(const struct cpu *cpu, const uint16_t addr);
static inline void cpu_update_paging(struct cpu *cpu, const uint16_t csr_addr)
{
    if (csr_addr != SATP)
        return;

    cpu->pagetable =
        (cpu_load_csr(cpu, SATP) & (((uint64_t) 1 << 44) - 1)) * PAGE_SIZE;
    cpu->enable_paging = (8 == (cpu_load_csr(cpu, SATP) >> 60));
}

exception_t cpu_translate(const struct cpu *cpu,
                          const uint64_t addr,
                          const exception_t e,
                          uint64_t *result)
{
    if (!cpu->enable_paging) {
        *result = addr;
        return OK;
    }

    const uint64_t vpn[] = {
        (addr >> 12) & 0x1ff,
        (addr >> 21) & 0x1ff,
        (addr >> 30) & 0x1ff,
    };
    int level = sizeof(vpn) / sizeof(vpn[0]) - 1;

    uint64_t a = cpu->pagetable;
    uint64_t pte;
    while (1) {
        exception_t exc = bus_load(cpu->bus, a + vpn[level] * 8, 64, &pte);
        if (exc != OK)
            return exc;
        bool v = pte & 1;
        bool r = (pte >> 1) & 1, w = (pte >> 2) & 1, x = (pte >> 3) & 1;
        if (!v || (!r && w))
            return e;

        if (r || x)
            break;

        a = ((pte >> 10) & 0x0fffffffffff) * PAGE_SIZE;
        if (--level < 0)
            return e;
    }

    const uint64_t ppn[] = {
        (pte >> 10) & 0x1ff,
        (pte >> 19) & 0x1ff,
        (pte >> 28) & 0x03ffffff,
    };

    uint64_t offset = addr & 0xfff;
    switch (level) {
    case 0:
        *result = (((pte >> 10) & 0x0fffffffffff) << 12) | offset;
        return OK;
    case 1:
        *result = (ppn[2] << 30) | (ppn[1] << 21) | (vpn[0] << 12) | offset;
        return OK;
    case 2:
        *result = (ppn[2] << 30) | (vpn[1] << 21) | (vpn[0] << 12) | offset;
        return OK;
    default:
        return e;
    }
}

/* Fetch an instruction from current PC from RAM. */
exception_t cpu_fetch(struct cpu *cpu, uint64_t *result)
{
    uint64_t ppc;
    exception_t e = cpu_translate(cpu, cpu->pc, INSTRUCTION_PAGE_FAULT, &ppc);
    if (e != OK)
        return e;

    if (bus_load(cpu->bus, ppc, 32, result) != OK)
        return INSTRUCTION_ACCESS_FAULT;
    return OK;
}

static inline uint64_t cpu_load_csr(const struct cpu *cpu, const uint16_t addr)
{
    if (addr == SIE)
        return cpu->csrs[MIE] & cpu->csrs[MIDELEG];
    return cpu->csrs[addr];
}

static inline void cpu_store_csr(struct cpu *cpu,
                                 const uint16_t addr,
                                 const uint64_t value)
{
    if (addr == SIE) {
        cpu->csrs[MIE] = (cpu->csrs[MIE] & ~cpu->csrs[MIDELEG]) |
                         (value & cpu->csrs[MIDELEG]);
        return;
    }
    cpu->csrs[addr] = value;
}

static inline exception_t cpu_load(struct cpu *cpu,
                                   const uint64_t addr,
                                   const uint64_t size,
                                   uint64_t *result)
{
    uint64_t pa;
    exception_t e = cpu_translate(cpu, addr, LOAD_PAGE_FAULT, &pa);
    if (e != OK)
        return e;
    return bus_load(cpu->bus, pa, size, result);
}

static inline exception_t cpu_store(struct cpu *cpu,
                                    const uint64_t addr,
                                    const uint64_t size,
                                    const uint64_t value)
{
    uint64_t pa;
    exception_t e = cpu_translate(cpu, addr, STORE_AMO_PAGE_FAULT, &pa);
    if (e != OK)
        return e;

    return bus_store(cpu->bus, pa, size, value);
}

exception_t cpu_execute(struct cpu *cpu, const uint64_t insn)
{
    uint64_t opcode = insn & 0x7f;
    uint64_t rd = (insn >> 7) & 0x1f;
    uint64_t rs1 = (insn >> 15) & 0x1f, rs2 = (insn >> 20) & 0x1f;
    uint64_t funct3 = (insn >> 12) & 0x7, funct7 = (insn >> 25) & 0x7f;

    cpu->regs[0] = 0; /* x0 register is always zero */

    exception_t e;
    switch (opcode) {
    case 0x03: {
        uint64_t imm = (int32_t) insn >> 20;
        uint64_t addr = cpu->regs[rs1] + imm;
        switch (funct3) {
        case 0x0: /* lb */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 8, &result)) != OK)
                return e;
            cpu->regs[rd] = (int8_t) result;
            break;
        }
        case 0x1: /* lh */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 16, &result)) != OK)
                return e;
            cpu->regs[rd] = (int16_t) result;
            break;
        }
        case 0x2: /* lw */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 32, &result)) != OK)
                return e;
            cpu->regs[rd] = (int32_t) result;
            break;
        }
        case 0x3: /* ld */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 64, &result)) != OK)
                return e;
            cpu->regs[rd] = result;
            break;
        }
        case 0x4: /* lbu */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 8, &result)) != OK)
                return e;
            cpu->regs[rd] = result;
            break;
        }
        case 0x5: /* lhu */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 16, &result)) != OK)
                return e;
            cpu->regs[rd] = result;
            break;
        }
        case 0x6: /* lwu */ {
            uint64_t result;
            if ((e = cpu_load(cpu, addr, 32, &result)) != OK)
                return e;
            cpu->regs[rd] = result;
            break;
        }
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x0f:
        switch (funct3) {
        case 0x0: /* fence */
            /* TODO: implemented */
            break;
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    case 0x13: {
        uint64_t imm = (int32_t) (insn & 0xfff00000) >> 20;
        uint32_t shamt = imm & 0x3f;

        switch (funct3) {
        case 0x0: /* addi */
            cpu->regs[rd] = cpu->regs[rs1] + imm;
            break;
        case 0x1: /* slli */
            cpu->regs[rd] = cpu->regs[rs1] << shamt;
            break;
        case 0x2: /* slti */
            cpu->regs[rd] = !!((int64_t) cpu->regs[rs1] < (int64_t) imm);
            break;
        case 0x3: /* sltiu */
            cpu->regs[rd] = !!(cpu->regs[rs1] < imm);
            break;
        case 0x4: /* xori */
            cpu->regs[rd] = cpu->regs[rs1] ^ imm;
            break;
        case 0x5:
            switch (funct7 >> 1) {
            case 0x00: /* srli */
                cpu->regs[rd] = cpu->regs[rs1] >> shamt;
                break;
            case 0x10: /* srai */
                cpu->regs[rd] = (int64_t) (cpu->regs[rs1]) >> shamt;
                break;
            default:
                return ILLEGAL_INSTRUCTION;
            }
            break;
        case 0x6: /* ori */
            cpu->regs[rd] = cpu->regs[rs1] | imm;
            break;
        case 0x7: /* andi */
            cpu->regs[rd] = cpu->regs[rs1] & imm;
            break;
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x17: /* auipc */ {
        uint64_t imm = (int32_t) (insn & 0xfffff000);
        cpu->regs[rd] = cpu->pc + imm - 4;
        break;
    }
    case 0x1b: {
        uint64_t imm = (int32_t) insn >> 20;
        uint32_t shamt = imm & 0x1f;
        switch (funct3) {
        case 0x0: /* addiw */
            cpu->regs[rd] = (int32_t) (cpu->regs[rs1] + imm);
            break;
        case 0x1: /* slliw */
            cpu->regs[rd] = (int32_t) (cpu->regs[rs1] << shamt);
            break;
        case 0x5: {
            switch (funct7) {
            case 0x00: /* srliw */
                cpu->regs[rd] = (int32_t) ((uint32_t) cpu->regs[rs1] >> shamt);
                break;
            case 0x20: /* sraiw */
                cpu->regs[rd] = (int32_t) (cpu->regs[rs1]) >> shamt;
                break;
            default:
                return ILLEGAL_INSTRUCTION;
            }
            break;
        }
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x23: {
        uint64_t imm = (uint64_t) ((int32_t) (insn & 0xfe000000) >> 20) |
                       ((insn >> 7) & 0x1f);
        uint64_t addr = cpu->regs[rs1] + imm;
        switch (funct3) {
        case 0x0: /* sb */
            if ((e = cpu_store(cpu, addr, 8, cpu->regs[rs2])) != OK)
                return e;
            break;
        case 0x1: /* sh */
            if ((e = cpu_store(cpu, addr, 16, cpu->regs[rs2])) != OK)
                return e;
            break;
        case 0x2: /* sw */
            if ((e = cpu_store(cpu, addr, 32, cpu->regs[rs2])) != OK)
                return e;
            break;
        case 0x3: /* sd */
            if ((e = cpu_store(cpu, addr, 64, cpu->regs[rs2])) != OK)
                return e;
            break;
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x2f: {
        uint64_t funct5 = (funct7 & 0x7c) >> 2;
        if (funct3 == 0x2 && funct5 == 0x00) { /* amoadd.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, t + cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x01) { /* amoswap.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, cpu->regs[rs2])) != OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x04) { /* amoxor.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, t ^ cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x08) { /* amoor.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, t | cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x0c) { /* amoand.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, t & cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x10) { /* amomin.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            int32_t min = MIN((int32_t) t, (int32_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, min)) != OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x14) { /* amomax.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            int32_t max = MAX((int32_t) t, (int32_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, max)) != OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x18) { /* amominu.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            uint32_t min = MIN((uint32_t) t, (uint32_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, min)) != OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x2 && funct5 == 0x1c) { /* amomaxu.w */
            if (!IS_ALIGNED(cpu->regs[rs1], 4))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 32, &t)) != OK)
                return e;
            uint32_t max = MAX((uint32_t) t, (uint32_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 32, max)) != OK)
                return e;
            cpu->regs[rd] = (int32_t) t;
        } else if (funct3 == 0x3 && funct5 == 0x00) { /* amoadd.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, t + cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x01) { /* amoswap.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, cpu->regs[rs2])) != OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x04) { /* amoxor.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, t ^ cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x08) { /* amoor.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, t | cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x0c) { /* amoand.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, t & cpu->regs[rs2])) !=
                OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x10) { /* amomin.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            int64_t min = MIN((int64_t) t, (int64_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, min)) != OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x14) { /* amomax.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            int64_t max = MAX((int64_t) t, (int64_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, max)) != OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x18) { /* amominu.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t, min;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            min = MIN((uint64_t) t, (uint64_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, min)) != OK)
                return e;
            cpu->regs[rd] = t;
        } else if (funct3 == 0x3 && funct5 == 0x1c) { /* amomaxu.d */
            if (!IS_ALIGNED(cpu->regs[rs1], 8))
                return LOAD_ADDRESS_MISALIGNED;
            uint64_t t, max;
            if ((e = cpu_load(cpu, cpu->regs[rs1], 64, &t)) != OK)
                return e;
            max = MAX((uint64_t) t, (uint64_t) cpu->regs[rs2]);
            if ((e = cpu_store(cpu, cpu->regs[rs1], 64, max)) != OK)
                return e;
            cpu->regs[rd] = t;
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x33: {
        uint32_t shamt = cpu->regs[rs2] & 0x3f;
        if (funct3 == 0x0 && funct7 == 0x00) { /* add */
            cpu->regs[rd] = cpu->regs[rs1] + cpu->regs[rs2];
        } else if (funct3 == 0x0 && funct7 == 0x01) { /* mul */
            cpu->regs[rd] = cpu->regs[rs1] * cpu->regs[rs2];
        } else if (funct3 == 0x0 && funct7 == 0x20) { /* sub */
            cpu->regs[rd] = cpu->regs[rs1] - cpu->regs[rs2];
        } else if (funct3 == 0x1 && funct7 == 0x00) { /* sll */
            cpu->regs[rd] = cpu->regs[rs1] << shamt;
        } else if (funct3 == 0x1 && funct7 == 0x01) { /* mulh */
#if defined(__SIZEOF_INT128__) && __SIZEOF_INT128__
            cpu->regs[rd] = ((__int128) (int64_t) cpu->regs[rs1] *
                             (__int128) (int64_t) cpu->regs[rs2]) >>
                            64;
#else
            cpu->regs[rd] =
                mulh((int64_t) cpu->regs[rs1], (int64_t) cpu->regs[rs2]);
#endif
        } else if (funct3 == 0x2 && funct7 == 0x00) { /* slt */
            cpu->regs[rd] =
                !!((int64_t) cpu->regs[rs1] < (int64_t) cpu->regs[rs2]);
        } else if (funct3 == 0x2 && funct7 == 0x01) { /* mulhsu */
#if defined(__SIZEOF_INT128__) && __SIZEOF_INT128__
            cpu->regs[rd] = ((__int128) (int64_t) cpu->regs[rs1] *
                             (unsigned __int128) cpu->regs[rs2]) >>
                            64;
#else
            cpu->regs[rd] =
                mulhsu((int64_t) cpu->regs[rs1], (uint64_t) cpu->regs[rs2]);
#endif
        } else if (funct3 == 0x3 && funct7 == 0x00) { /* sltu */
            cpu->regs[rd] = !!(cpu->regs[rs1] < cpu->regs[rs2]);
        } else if (funct3 == 0x3 && funct7 == 0x01) { /* mulhu */
#if defined(__SIZEOF_INT128__) && __SIZEOF_INT128__
            cpu->regs[rd] = ((unsigned __int128) cpu->regs[rs1] *
                             (unsigned __int128) cpu->regs[rs2]) >>
                            64;
#else
            cpu->regs[rd] =
                mulhu((uint64_t) cpu->regs[rs1], (uint64_t) cpu->regs[rs2]);

#endif
        } else if (funct3 == 0x4 && funct7 == 0x00) { /* xor */
            cpu->regs[rd] = cpu->regs[rs1] ^ cpu->regs[rs2];
        } else if (funct3 == 0x4 && funct7 == 0x01) { /* div */
            int64_t dividend = (int64_t) cpu->regs[rs1];
            int64_t divisor = (int64_t) cpu->regs[rs2];
            if (divisor == 0) {
                cpu->regs[rd] = -1;
            } else if (dividend == INT64_MIN && divisor == -1) { /* overflow */
                cpu->regs[rd] = INT64_MIN;
            } else {
                cpu->regs[rd] = dividend / divisor;
            }
        } else if (funct3 == 0x5 && funct7 == 0x00) { /* srl */
            cpu->regs[rd] = cpu->regs[rs1] >> shamt;
        } else if (funct3 == 0x5 && funct7 == 0x01) { /* divu */
            cpu->regs[rd] = (cpu->regs[rs2] == 0)
                                ? UINT64_MAX
                                : (cpu->regs[rs1] / cpu->regs[rs2]);
        } else if (funct3 == 0x5 && funct7 == 0x20) { /* sra */
            cpu->regs[rd] = (int64_t) cpu->regs[rs1] >> shamt;
        } else if (funct3 == 0x6 && funct7 == 0x01) { /* rem */
            if (cpu->regs[rs2] == 0) {
                cpu->regs[rd] = cpu->regs[rs1];
            } else if ((int64_t) cpu->regs[rs1] == INT64_MIN &&
                       (int64_t) cpu->regs[rs2] == -1) { /* overflow */
                cpu->regs[rd] = 0;
            } else {
                cpu->regs[rd] =
                    (int64_t) cpu->regs[rs1] % (int64_t) cpu->regs[rs2];
            }
        } else if (funct3 == 0x6 && funct7 == 0x00) { /* or */
            cpu->regs[rd] = cpu->regs[rs1] | cpu->regs[rs2];
        } else if (funct3 == 0x7 && funct7 == 0x00) { /* and */
            cpu->regs[rd] = cpu->regs[rs1] & cpu->regs[rs2];
        } else if (funct3 == 0x7 && funct7 == 0x01) { /* remu */
            cpu->regs[rd] = (cpu->regs[rs2] == 0)
                                ? cpu->regs[rs1]
                                : (cpu->regs[rs1] % cpu->regs[rs2]);
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x37: /* lui */
        cpu->regs[rd] = (int32_t) (insn & 0xfffff000);
        break;
    case 0x3b: {
        uint32_t shamt = cpu->regs[rs2] & 0x1f;
        if (funct3 == 0x0 && funct7 == 0x00) { /* addw */
            cpu->regs[rd] = (int32_t) (cpu->regs[rs1] + cpu->regs[rs2]);
        } else if (funct3 == 0x0 && funct7 == 0x01) { /* mulw */
            cpu->regs[rd] = (int32_t) cpu->regs[rs1] * (int32_t) cpu->regs[rs2];
        } else if (funct3 == 0x0 && funct7 == 0x20) { /* subw */
            cpu->regs[rd] = (int32_t) (cpu->regs[rs1] - cpu->regs[rs2]);
        } else if (funct3 == 0x1 && funct7 == 0x00) { /* sllw */
            cpu->regs[rd] = (int32_t) ((uint32_t) cpu->regs[rs1] << shamt);
        } else if (funct3 == 0x4 && funct7 == 0x01) { /* divw */
            if (cpu->regs[rs2] == 0) {
                cpu->regs[rd] = -1;
            } else if ((int32_t) cpu->regs[rs1] == INT32_MIN &&
                       (int32_t) cpu->regs[rs2] == -1) { /* overflow */
                cpu->regs[rd] = INT32_MIN;
            } else {
                cpu->regs[rd] =
                    (int32_t) cpu->regs[rs1] / (int32_t) cpu->regs[rs2];
            }
        } else if (funct3 == 0x5 && funct7 == 0x00) { /* srlw */
            cpu->regs[rd] = (int32_t) ((uint32_t) cpu->regs[rs1] >> shamt);
        } else if (funct3 == 0x5 && funct7 == 0x01) { /* divuw */
            cpu->regs[rd] = (cpu->regs[rs2] == 0)
                                ? UINT64_MAX
                                : (int32_t) ((uint32_t) cpu->regs[rs1] /
                                             (uint32_t) cpu->regs[rs2]);
        } else if (funct3 == 0x5 && funct7 == 0x20) { /* sraw */
            cpu->regs[rd] = (int32_t) cpu->regs[rs1] >> (int32_t) shamt;
        } else if (funct3 == 0x6 && funct7 == 0x01) { /* remw */
            if (cpu->regs[rs2] == 0) {
                cpu->regs[rd] = cpu->regs[rs1];
            } else if ((int32_t) cpu->regs[rs1] == INT32_MIN &&
                       (int32_t) cpu->regs[rs2] == -1) { /* overflow */
                cpu->regs[rd] = 0;
            } else {
                cpu->regs[rd] =
                    (int32_t) cpu->regs[rs1] % (int32_t) cpu->regs[rs2];
            }
        } else if (funct3 == 0x7 && funct7 == 0x01) { /* remuw */
            cpu->regs[rd] = (cpu->regs[rs2] == 0)
                                ? cpu->regs[rs1]
                                : (int32_t) ((uint32_t) cpu->regs[rs1] %
                                             (uint32_t) cpu->regs[rs2]);
        } else {
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x63: {
        uint64_t imm = (uint64_t) ((int32_t) (insn & 0x80000000) >> 19) |
                       ((insn & 0x80) << 4) | ((insn >> 20) & 0x7e0) |
                       ((insn >> 7) & 0x1e);

        switch (funct3) {
        case 0x0: /* beq */
            if (cpu->regs[rs1] == cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x1: /* bne */
            if (cpu->regs[rs1] != cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x4: /* blt */
            if ((int64_t) cpu->regs[rs1] < (int64_t) cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x5: /* bge */
            if ((int64_t) cpu->regs[rs1] >= (int64_t) cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x6: /* bltu */
            if (cpu->regs[rs1] < cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        case 0x7: /* bgeu */
            if (cpu->regs[rs1] >= cpu->regs[rs2])
                cpu->pc += imm - 4;
            break;
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    case 0x67: { /* jalr */
        uint64_t t = cpu->pc;
        uint64_t imm = (int32_t) (insn & 0xfff00000) >> 20;
        cpu->pc = (cpu->regs[rs1] + imm) & ~1;

        cpu->regs[rd] = t;
        break;
    }
    case 0x6f: { /* jal */
        cpu->regs[rd] = cpu->pc;

        uint64_t imm = (uint64_t) ((int32_t) (insn & 0x80000000) >> 11) |
                       (insn & 0xff000) | ((insn >> 9) & 0x800) |
                       ((insn >> 20) & 0x7fe);

        cpu->pc += imm - 4;
        break;
    }
    case 0x73: {
        uint16_t addr = (insn & 0xfff00000) >> 20;
        switch (funct3) {
        case 0x0:
            if (rs2 == 0x0 && funct7 == 0x0) { /* ecall */
                switch (cpu->mode) {
                case USER:
                case SUPERVISOR:
                case MACHINE:
                    return 8 + cpu->mode; /* ECALL_FROM_{U,S,M}MODE */
                }
            } else if (rs2 == 0x1 && funct7 == 0x0) { /* ebreak */
                return BREAKPOINT;
            } else if (rs2 == 0x2 && funct7 == 0x8) { /* sret */
                cpu->pc = cpu_load_csr(cpu, SEPC);
                cpu->mode =
                    ((cpu_load_csr(cpu, SSTATUS) >> 8) & 1) ? SUPERVISOR : USER;
                cpu_store_csr(cpu, SSTATUS,
                              ((cpu_load_csr(cpu, SSTATUS) >> 5) & 1)
                                  ? cpu_load_csr(cpu, SSTATUS) | (1 << 1)
                                  : cpu_load_csr(cpu, SSTATUS) & ~(1 << 1));
                cpu_store_csr(cpu, SSTATUS,
                              cpu_load_csr(cpu, SSTATUS) | (1 << 5));
                cpu_store_csr(cpu, SSTATUS,
                              cpu_load_csr(cpu, SSTATUS) & ~(1 << 8));
            } else if (rs2 == 0x2 && funct7 == 0x18) { /* mret */
                cpu->pc = cpu_load_csr(cpu, MEPC);
                uint64_t mpp = (cpu_load_csr(cpu, MSTATUS) >> 11) & 3;
                cpu->mode = mpp == 2 ? MACHINE : (mpp == 1 ? SUPERVISOR : USER);
                cpu_store_csr(cpu, MSTATUS,
                              ((cpu_load_csr(cpu, MSTATUS) >> 7) & 1)
                                  ? cpu_load_csr(cpu, MSTATUS) | (1 << 3)
                                  : cpu_load_csr(cpu, MSTATUS) & ~(1 << 3));
                cpu_store_csr(cpu, MSTATUS,
                              cpu_load_csr(cpu, MSTATUS) | (1 << 7));
                cpu_store_csr(cpu, MSTATUS,
                              cpu_load_csr(cpu, MSTATUS) & ~(3 << 11));
            } else if (funct7 == 0x9) { /* sfence.vma */
                /* TODO: implemented */
            } else {
                return ILLEGAL_INSTRUCTION;
            }
            break;
        case 0x1: { /* csrrw */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x2: { /* csrrs */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t | cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x3: { /* csrrc */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t & ~cpu->regs[rs1]);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x5: /* csrrwi */
            cpu->regs[rd] = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, rs1);
            cpu_update_paging(cpu, addr);
            break;
        case 0x6: { /* csrrsi */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t | rs1);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        case 0x7: { /* csrrci */
            uint64_t t = cpu_load_csr(cpu, addr);
            cpu_store_csr(cpu, addr, t & ~rs1);
            cpu->regs[rd] = t;
            cpu_update_paging(cpu, addr);
            break;
        }
        default:
            return ILLEGAL_INSTRUCTION;
        }
        break;
    }
    default:
        return ILLEGAL_INSTRUCTION;
    }

    return OK;
}

void cpu_take_trap(struct cpu *cpu, const exception_t e, const interrupt_t intr)
{
    uint64_t exception_pc = cpu->pc - 4;
    cpu_mode_t prev_mode = cpu->mode;

    bool is_interrupt = (intr != NONE);
    uint64_t cause = e;
    if (is_interrupt)
        cause = ((uint64_t) 1 << 63) | (uint64_t) intr;

    if (prev_mode <= SUPERVISOR &&
        (((cpu_load_csr(cpu, MEDELEG) >> (uint32_t) cause) & 1) != 0)) {
        cpu->mode = SUPERVISOR;
        if (is_interrupt) {
            uint64_t vec = (cpu_load_csr(cpu, STVEC) & 1) ? (4 * cause) : 0;
            cpu->pc = (cpu_load_csr(cpu, STVEC) & ~1) + vec;
        } else {
            cpu->pc = cpu_load_csr(cpu, STVEC) & ~1;
        }
        cpu_store_csr(cpu, SEPC, exception_pc & ~1);
        cpu_store_csr(cpu, SCAUSE, cause);
        cpu_store_csr(cpu, STVAL, 0);
        cpu_store_csr(cpu, SSTATUS,
                      ((cpu_load_csr(cpu, SSTATUS) >> 1) & 1)
                          ? cpu_load_csr(cpu, SSTATUS) | (1 << 5)
                          : cpu_load_csr(cpu, SSTATUS) & ~(1 << 5));
        cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) & ~(1 << 1));
        if (prev_mode == USER) {
            cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) & ~(1 << 8));
        } else {
            cpu_store_csr(cpu, SSTATUS, cpu_load_csr(cpu, SSTATUS) | (1 << 8));
        }
    } else {
        cpu->mode = MACHINE;

        if (is_interrupt) {
            uint64_t vec = (cpu_load_csr(cpu, MTVEC) & 1) ? 4 * cause : 0;
            cpu->pc = (cpu_load_csr(cpu, MTVEC) & ~1) + vec;
        } else {
            cpu->pc = cpu_load_csr(cpu, MTVEC) & ~1;
        }
        cpu_store_csr(cpu, MEPC, exception_pc & ~1);
        cpu_store_csr(cpu, MCAUSE, cause);
        cpu_store_csr(cpu, MTVAL, 0);
        cpu_store_csr(cpu, MSTATUS,
                      ((cpu_load_csr(cpu, MSTATUS) >> 3) & 1)
                          ? cpu_load_csr(cpu, MSTATUS) | (1 << 7)
                          : cpu_load_csr(cpu, MSTATUS) & ~(1 << 7));
        cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) & ~(1 << 3));
        cpu_store_csr(cpu, MSTATUS, cpu_load_csr(cpu, MSTATUS) & ~(3 << 11));
    }
}

enum { DISK_IRQ = 1, UART_IRQ = 10 };

interrupt_t cpu_check_pending_interrupt(struct cpu *cpu)
{
    if (cpu->mode == MACHINE && ((cpu_load_csr(cpu, MSTATUS) >> 3) & 1) == 0)
        return NONE;
    if (cpu->mode == SUPERVISOR && ((cpu_load_csr(cpu, SSTATUS) >> 1) & 1) == 0)
        return NONE;

    do {
        uint64_t irq;
        if (uart_is_interrupting(cpu->bus->uart)) {
            irq = UART_IRQ;
        } else if (disk_is_interrupting(cpu->bus->disk)) {
            bus_disk_access(cpu->bus);
            irq = DISK_IRQ;
        } else
            break;

        bus_store(cpu->bus, PLIC_SCLAIM, 32, irq);
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) | MIP_SEIP);
    } while (0);

    uint64_t pending = cpu_load_csr(cpu, MIE) & cpu_load_csr(cpu, MIP);
    if (pending & MIP_MEIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MEIP);
        return MACHINE_EXTERNAL_INTERRUPT;
    }
    if (pending & MIP_MSIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MSIP);
        return MACHINE_SOFTWARE_INTERRUPT;
    }
    if (pending & MIP_MTIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_MTIP);
        return MACHINE_TIMER_INTERRUPT;
    }
    if (pending & MIP_SEIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_SEIP);
        return SUPERVISOR_EXTERNAL_INTERRUPT;
    }
    if (pending & MIP_SSIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_SSIP);
        return SUPERVISOR_SOFTWARE_INTERRUPT;
    }
    if (pending & MIP_STIP) {
        cpu_store_csr(cpu, MIP, cpu_load_csr(cpu, MIP) & ~MIP_STIP);
        return SUPERVISOR_TIMER_INTERRUPT;
    }

    return NONE;
}

size_t read_file(FILE *f, uint8_t *r[])
{
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *content = malloc(fsize + 1);
    if (fread(content, fsize, 1, f) != 1) /* less than fsize bytes */
        fatal("read file content");

    content[fsize] = 0;
    *r = content;

    return fsize;
}
