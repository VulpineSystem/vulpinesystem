#pragma once

#define CPU_HZ 33000000

#define N_REG 32
#define N_CSR 4096

#define RAM_SIZE (1024 * 1024 * 8)
#define RAM_BASE 0x80000000

#define FRAMEBUFFER_BASE 0x80600000

#define CLINT_BASE 0x2000000
#define CLINT_SIZE 0x10000
#define CLINT_MTIMECMP (CLINT_BASE + 0x4000)
#define CLINT_MTIME (CLINT_BASE + 0xbff8)

#define PLIC_BASE 0xC000000
#define PLIC_SIZE 0x4000000
#define PLIC_PENDING (PLIC_BASE + 0x1000)
#define PLIC_SENABLE (PLIC_BASE + 0x2080)
#define PLIC_SPRIORITY (PLIC_BASE + 0x201000)
#define PLIC_SCLAIM (PLIC_BASE + 0x201004)

#define UART_BASE 0x10000000
#define UART_SIZE 0x100
enum { UART_RHR = UART_BASE, UART_THR = UART_BASE };
enum { UART_LCR = UART_BASE + 3, UART_LSR = UART_BASE + 5 };
enum { UART_LSR_RX = 1, UART_LSR_TX = 1 << 5 };

#define DISK_BASE 0x10001000
#define DISK_SIZE 0x100
#define DISK_MAGIC (DISK_BASE + 0x000)
#define DISK_VERSION (DISK_BASE + 0x004)
#define DISK_NOTIFY (DISK_BASE + 0x008)
#define DISK_DIRECTION (DISK_BASE + 0x00C)
#define DISK_BUFFER_ADDR_HIGH (DISK_BASE + 0x010)
#define DISK_BUFFER_ADDR_LOW (DISK_BASE + 0x014)
#define DISK_BUFFER_LEN_HIGH (DISK_BASE + 0x018)
#define DISK_BUFFER_LEN_LOW (DISK_BASE + 0x01C)
#define DISK_SECTOR (DISK_BASE + 0x020)

/* USER is a mode for application which runs on operating system.
 * SUPERVISOR is a mode for operating system.
 * MACHINE is a mode for RISC-V hart internal operation, sometimes called
 * kernal-mode or protect-mode in other architecture.
 */
typedef enum { USER = 0x0, SUPERVISOR = 0x1, MACHINE = 0x3 } cpu_mode_t;

struct cpu {
    uint64_t regs[N_REG], pc;
    uint64_t csrs[N_CSR];
    cpu_mode_t mode;
    struct bus *bus;
    bool enable_paging;
    uint64_t pagetable;
};

struct bus {
    struct ram *ram;
    struct clint *clint;
    struct plic *plic;
    struct uart *uart;
    struct disk *disk;
};

struct ram {
    uint8_t *data;
};

struct clint {
    uint64_t mtime, mtimecmp;
};

struct plic {
    uint64_t pending;
    uint64_t senable;
    uint64_t spriority;
    uint64_t sclaim;
};

struct uart {
    uint8_t data[UART_SIZE];
    bool interrupting;

    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct disk {
    uint32_t buffer_address_high;
    uint32_t buffer_address_low;
    uint32_t buffer_length_high;
    uint32_t buffer_length_low;
    uint32_t sector;
    uint32_t notify;
    uint32_t direction;
    uint8_t *disk;
};

typedef enum {
    OK = -1,
    INSTRUCTION_ADDRESS_MISALIGNED = 0,
    INSTRUCTION_ACCESS_FAULT = 1,
    ILLEGAL_INSTRUCTION = 2,
    BREAKPOINT = 3,
    LOAD_ADDRESS_MISALIGNED = 4,
    LOAD_ACCESS_FAULT = 5,
    STORE_AMO_ADDRESS_MISALIGNED = 6,
    STORE_AMO_ACCESS_FAULT = 7,
    INSTRUCTION_PAGE_FAULT = 12,
    LOAD_PAGE_FAULT = 13,
    STORE_AMO_PAGE_FAULT = 15,
} exception_t;

typedef enum {
    NONE = -1,
    SUPERVISOR_SOFTWARE_INTERRUPT = 1,
    MACHINE_SOFTWARE_INTERRUPT = 3,
    SUPERVISOR_TIMER_INTERRUPT = 5,
    MACHINE_TIMER_INTERRUPT = 7,
    SUPERVISOR_EXTERNAL_INTERRUPT = 9,
    MACHINE_EXTERNAL_INTERRUPT = 11,
} interrupt_t;

void fatal(const char *msg);
bool exception_is_fatal(const exception_t e);
size_t read_file(FILE *f, uint8_t *r[]);
struct cpu *cpu_new(uint8_t *code, const size_t code_size, uint8_t *disk);
exception_t cpu_fetch(struct cpu *cpu, uint64_t *result);
void cpu_take_trap(struct cpu *cpu, const exception_t e, const interrupt_t intr);
exception_t cpu_execute(struct cpu *cpu, const uint64_t insn);
interrupt_t cpu_check_pending_interrupt(struct cpu *cpu);
