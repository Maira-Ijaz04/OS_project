/*
 * ============================================================
 * PROJECT : Mini OS Simulator + Concurrent Railway Reservation
 * COURSE  : CL-2006 Operating Systems Lab - Final Project
 * LANGUAGE: C (POSIX threads, semaphores, shared memory)
 * ============================================================
 * COMPILE : gcc main.c -o railway_os -lpthread -lrt
 * RUN     : ./railway_os
 * ============================================================
 * DESCRIPTION:
 *   This program simulates a Mini Operating System featuring:
 *     - Boot simulation with resource initialization
 *     - Process creation, PCB management, and process states
 *     - Ready queue with FCFS and Round Robin schedulers
 *     - Mutex and semaphore-based synchronization
 *     - Interrupt handling (timeout, cancellation, shutdown)
 *     - User Mode vs Kernel Mode access control
 *     - Concurrent Railway Reservation System as core application
 *     - File-based persistent storage and activity logging
 *     - Inter-Process Communication via shared memory (simulated)
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */
#define MAX_SEATS        50
#define MAX_PROCESSES    100
#define MAX_NAME_LEN     64
#define MAX_ID_LEN       32
#define MAX_DATE_LEN     16
#define PROCESS_MEM_MB   64
#define RR_QUANTUM_DEF   3
#define LOG_FILE         "system_log.txt"
#define BOOKING_FILE     "bookings.txt"
#define SEAT_FILE        "seats.txt"

/* ============================================================
 * ENUMERATIONS
 * ============================================================ */

/* Process lifecycle states */
typedef enum {
    STATE_NEW        = 0,
    STATE_READY      = 1,
    STATE_RUNNING    = 2,
    STATE_BLOCKED    = 3,
    STATE_TERMINATED = 4
} ProcessState;

/* CPU scheduling algorithms */
typedef enum {
    ALGO_FCFS        = 1,
    ALGO_ROUND_ROBIN = 2,
    ALGO_PRIORITY    = 3
} SchedAlgo;

/* User/Kernel mode */
typedef enum {
    MODE_USER   = 0,
    MODE_KERNEL = 1
} SystemMode;

/* Interrupt types */
typedef enum {
    INT_NONE         = 0,
    INT_CANCELLATION = 1,
    INT_TIMEOUT      = 2,
    INT_SHUTDOWN     = 3
} InterruptType;

/* ============================================================
 * DATA STRUCTURES
 * ============================================================ */

/*
 * STRUCT: PCB (Process Control Block)
 * Holds all information about a process/thread that represents
 * a passenger request in the system.
 */
typedef struct {
    int          pid;
    char         process_name[MAX_NAME_LEN];
    ProcessState state;
    int          priority;
    int          burst_time;         /* total CPU burst in ms (simulated) */
    int          remaining_time;     /* remaining burst for Round Robin   */
    int          arrival_time;       /* unix timestamp at creation        */
    int          wait_time;
    int          turnaround_time;
    int          memory_mb;
    int          cpu_core;           /* -1 = not assigned                 */
    char         passenger_name[MAX_NAME_LEN];
    char         request_type[16];   /* BOOK/CANCEL/VIEW/STATUS/INFO      */
    char         request_data[256];  /* pipe-delimited extra params       */
    InterruptType interrupt;
    int          active;             /* 1 = slot in use                   */
} PCB;

/*
 * STRUCT: Seat
 * Represents one seat in the train, stored in shared memory.
 */
typedef struct {
    int  seat_number;
    int  is_booked;
    char passenger_name[MAX_NAME_LEN];
    char passenger_id[MAX_ID_LEN];
    char booking_id[MAX_ID_LEN];
    char train_number[16];
    char journey_date[MAX_DATE_LEN];
    char seat_class[16];             /* Economy / Business / First        */
} Seat;

/*
 * STRUCT: BookingRecord
 * Persistent record saved to bookings.txt.
 */
typedef struct {
    char booking_id[MAX_ID_LEN];
    char passenger_name[MAX_NAME_LEN];
    char passenger_id[MAX_ID_LEN];
    int  seat_number;
    char train_number[16];
    char journey_date[MAX_DATE_LEN];
    char seat_class[16];
    char status[16];                 /* CONFIRMED / CANCELLED             */
    char timestamp[32];
} BookingRecord;

/*
 * STRUCT: SystemResources
 * Tracks RAM, Disk, and CPU availability.
 */
typedef struct {
    long total_ram_mb;
    long available_ram_mb;
    long total_disk_gb;
    long available_disk_gb;
    int  total_cores;
    int  available_cores;
    int  core_busy[8];
} SystemResources;

/*
 * STRUCT: ReadyQueue
 * Circular array-based queue storing PIDs.
 */
typedef struct {
    int pids[MAX_PROCESSES];
    int head;
    int tail;
    int count;
} ReadyQueue;

/*
 * STRUCT: PassengerArg
 * Argument passed to each passenger thread.
 */
typedef struct {
    int  pid;
    char request_type[16];
    char request_data[256];
    char passenger_name[MAX_NAME_LEN];
} PassengerArg;

/* ============================================================
 * GLOBAL STATE
 * ============================================================ */
PCB              g_proc_table[MAX_PROCESSES];
Seat             g_seats[MAX_SEATS];
BookingRecord    g_bookings[MAX_PROCESSES * 4];
int              g_booking_count  = 0;
int              g_next_pid       = 1;
int              g_next_booking   = 1000;
int              g_proc_count     = 0;
int              g_system_running = 0;
int              g_shutdown_req   = 0;
SchedAlgo        g_sched_algo     = ALGO_FCFS;
SystemMode       g_mode           = MODE_USER;
SystemResources  g_resources;
ReadyQueue       g_ready_queue;
int              g_rr_quantum     = RR_QUANTUM_DEF;

/* ── Synchronization Primitives ─────────────────────────────── */
pthread_mutex_t  g_seat_mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_proc_mtx      = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_log_mtx       = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_rq_mtx        = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_res_mtx       = PTHREAD_MUTEX_INITIALIZER;
sem_t            g_seat_sem;       /* binary semaphore: 1 thread in critical section */

/* ============================================================
 * UTILITY FUNCTIONS
 * ============================================================ */

/*
 * get_timestamp()
 * Fills buf with the current date-time string (YYYY-MM-DD HH:MM:SS).
 */
void get_timestamp(char *buf, int len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

/*
 * print_line()
 * Prints a horizontal divider line of given char and length.
 */
void print_line(char c, int len) {
    for (int i = 0; i < len; i++) putchar(c);
    putchar('\n');
}

/*
 * state_name()
 * Returns the string name of a ProcessState enum value.
 */
const char *state_name(ProcessState s) {
    switch (s) {
        case STATE_NEW:        return "NEW";
        case STATE_READY:      return "READY";
        case STATE_RUNNING:    return "RUNNING";
        case STATE_BLOCKED:    return "BLOCKED";
        case STATE_TERMINATED: return "TERMINATED";
        default:               return "UNKNOWN";
    }
}

/*
 * clear_screen()
 * Clears the terminal screen.
 */
void clear_screen(void) {
    printf("\033[2J\033[H");
}

/* ============================================================
 * LOGGER
 * ============================================================ */

/*
 * log_event()
 * Thread-safe: appends a timestamped entry to system_log.txt.
 * Parameters:
 *   level   - "INFO", "WARN", "ERROR", "INTERRUPT"
 *   message - Description string
 */
void log_event(const char *level, const char *message) {
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    pthread_mutex_lock(&g_log_mtx);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", ts, level, message);
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_mtx);
}

/*
 * log_proc_event()
 * Logs a process lifecycle event with PID, event label, and state.
 */
void log_proc_event(int pid, const char *event, ProcessState state) {
    char msg[128];
    snprintf(msg, sizeof(msg), "PID=%d EVENT=%s STATE=%s", pid, event, state_name(state));
    log_event("INFO", msg);
}

/* ============================================================
 * RESOURCE MANAGEMENT
 * ============================================================ */

/*
 * allocate_memory()
 * Deducts mb from available RAM if sufficient.
 * Thread-safe via g_res_mtx.
 * Returns 1 on success, 0 on failure.
 */
int allocate_memory(int mb) {
    pthread_mutex_lock(&g_res_mtx);
    int ok = (g_resources.available_ram_mb >= mb);
    if (ok) g_resources.available_ram_mb -= mb;
    pthread_mutex_unlock(&g_res_mtx);
    return ok;
}

/*
 * release_memory()
 * Returns mb to the available RAM pool, capped at total.
 */
void release_memory(int mb) {
    pthread_mutex_lock(&g_res_mtx);
    g_resources.available_ram_mb += mb;
    if (g_resources.available_ram_mb > g_resources.total_ram_mb)
        g_resources.available_ram_mb = g_resources.total_ram_mb;
    pthread_mutex_unlock(&g_res_mtx);
}

/*
 * allocate_core()
 * Finds a free CPU core, marks it busy, and returns its index.
 * Returns -1 if no core is free.
 */
int allocate_core(void) {
    pthread_mutex_lock(&g_res_mtx);
    int core = -1;
    for (int i = 0; i < g_resources.total_cores && i < 8; i++) {
        if (!g_resources.core_busy[i]) {
            g_resources.core_busy[i] = 1;
            g_resources.available_cores--;
            core = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_res_mtx);
    return core;
}

/*
 * release_core()
 * Marks the given CPU core as free and increments available_cores.
 */
void release_core(int core) {
    if (core < 0 || core >= 8) return;
    pthread_mutex_lock(&g_res_mtx);
    g_resources.core_busy[core] = 0;
    g_resources.available_cores++;
    pthread_mutex_unlock(&g_res_mtx);
}

/* ============================================================
 * READY QUEUE
 * ============================================================ */

/*
 * rq_enqueue()
 * Adds a PID to the back of the circular ready queue.
 * Thread-safe via g_rq_mtx.
 */
void rq_enqueue(int pid) {
    pthread_mutex_lock(&g_rq_mtx);
    if (g_ready_queue.count < MAX_PROCESSES) {
        g_ready_queue.pids[g_ready_queue.tail] = pid;
        g_ready_queue.tail = (g_ready_queue.tail + 1) % MAX_PROCESSES;
        g_ready_queue.count++;
    }
    pthread_mutex_unlock(&g_rq_mtx);
}

/*
 * rq_dequeue()
 * Removes and returns the front PID from the ready queue.
 * Returns -1 if the queue is empty.
 */
int rq_dequeue(void) {
    pthread_mutex_lock(&g_rq_mtx);
    int pid = -1;
    if (g_ready_queue.count > 0) {
        pid = g_ready_queue.pids[g_ready_queue.head];
        g_ready_queue.head = (g_ready_queue.head + 1) % MAX_PROCESSES;
        g_ready_queue.count--;
    }
    pthread_mutex_unlock(&g_rq_mtx);
    return pid;
}

/*
 * rq_size()
 * Returns the current number of items in the ready queue.
 */
int rq_size(void) {
    pthread_mutex_lock(&g_rq_mtx);
    int sz = g_ready_queue.count;
    pthread_mutex_unlock(&g_rq_mtx);
    return sz;
}

/* ============================================================
 * PROCESS MANAGEMENT
 * ============================================================ */

/*
 * find_pcb()
 * Returns a pointer to the PCB with the given PID, or NULL.
 * Caller must hold g_proc_mtx if modifying.
 */
PCB *find_pcb(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (g_proc_table[i].active && g_proc_table[i].pid == pid)
            return &g_proc_table[i];
    return NULL;
}

/*
 * find_free_pcb_slot()
 * Returns index of the first unused slot in g_proc_table, or -1.
 */
int find_free_pcb_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!g_proc_table[i].active) return i;
    return -1;
}

/*
 * create_process()
 * Allocates RAM, builds a PCB, and enqueues the process.
 * Each passenger request calls this to become a schedulable process.
 * Returns PID on success, -1 on failure.
 */
int create_process(const char *passenger, const char *req_type,
                   const char *req_data, int priority, int burst_ms)
{
    /* Step 1: Check RAM */
    if (!allocate_memory(PROCESS_MEM_MB)) {
        printf("[OS] ERROR: Not enough RAM to create process for %s\n", passenger);
        log_event("ERROR", "RAM exhausted for process creation");
        return -1;
    }

    pthread_mutex_lock(&g_proc_mtx);
    int slot = find_free_pcb_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&g_proc_mtx);
        release_memory(PROCESS_MEM_MB);
        printf("[OS] ERROR: Process table full.\n");
        return -1;
    }

    PCB *p = &g_proc_table[slot];
    memset(p, 0, sizeof(PCB));

    p->pid            = g_next_pid++;
    p->state          = STATE_NEW;
    p->priority       = priority;
    p->burst_time     = burst_ms;
    p->remaining_time = burst_ms;
    p->arrival_time   = (int)time(NULL);
    p->memory_mb      = PROCESS_MEM_MB;
    p->cpu_core       = -1;
    p->interrupt      = INT_NONE;
    p->active         = 1;
    p->wait_time      = 0;
    p->turnaround_time= 0;

    strncpy(p->passenger_name, passenger,  MAX_NAME_LEN - 1);
    strncpy(p->request_type,   req_type,   15);
    strncpy(p->request_data,   req_data,   255);
    snprintf(p->process_name, MAX_NAME_LEN, "%s_%s", req_type, passenger);

    int pid = p->pid;
    pthread_mutex_unlock(&g_proc_mtx);

    printf("[OS] Process created -> PID=%-4d  User=%-20s  Request=%s\n",
           pid, passenger, req_type);
    log_proc_event(pid, "CREATED", STATE_NEW);

    /* Step 2: Move to READY and enqueue */
    pthread_mutex_lock(&g_proc_mtx);
    PCB *pp = find_pcb(pid);
    if (pp) pp->state = STATE_READY;
    pthread_mutex_unlock(&g_proc_mtx);

    rq_enqueue(pid);
    log_proc_event(pid, "ENQUEUED", STATE_READY);

    return pid;
}

/*
 * terminate_process()
 * Sets a process to TERMINATED, releases its CPU core and memory.
 */
void terminate_process(int pid) {
    pthread_mutex_lock(&g_proc_mtx);
    PCB *p = find_pcb(pid);
    if (!p) { pthread_mutex_unlock(&g_proc_mtx); return; }

    p->state = STATE_TERMINATED;
    int core = p->cpu_core;
    int mem  = p->memory_mb;
    p->cpu_core = -1;
    pthread_mutex_unlock(&g_proc_mtx);

    if (core >= 0) release_core(core);
    release_memory(mem);

    log_proc_event(pid, "TERMINATED", STATE_TERMINATED);
    printf("[OS] PID=%-4d terminated. Core and RAM released.\n", pid);
}

/*
 * handle_interrupt()
 * Moves a process to BLOCKED, frees its CPU core,
 * and for TIMEOUT or SHUTDOWN also terminates it.
 */
void handle_interrupt(int pid, InterruptType itype) {
    const char *iname = (itype == INT_CANCELLATION) ? "CANCELLATION"
                      : (itype == INT_TIMEOUT)       ? "TIMEOUT"
                      : (itype == INT_SHUTDOWN)       ? "SHUTDOWN"
                      :                                "UNKNOWN";

    printf("\n[INTERRUPT] PID=%-4d  Type=%s\n", pid, iname);
    char msg[128];
    snprintf(msg, sizeof(msg), "PID=%d Type=%s", pid, iname);
    log_event("INTERRUPT", msg);

    pthread_mutex_lock(&g_proc_mtx);
    PCB *p = find_pcb(pid);
    if (p) {
        p->state = STATE_BLOCKED;
        int core = p->cpu_core;
        p->cpu_core = -1;
        pthread_mutex_unlock(&g_proc_mtx);
        if (core >= 0) release_core(core);
    } else {
        pthread_mutex_unlock(&g_proc_mtx);
    }

    if (itype == INT_TIMEOUT || itype == INT_SHUTDOWN)
        terminate_process(pid);
}

/* ============================================================
 * INTER-PROCESS COMMUNICATION (IPC) - SHARED MEMORY SIMULATION
 * ============================================================
 * Uses a global shared memory region (simulating POSIX shm)
 * to pass booking notifications between processes/threads.
 * In a real OS this would use shm_open / mmap; here we model
 * the same semantics with a mutex-protected ring buffer.
 * ============================================================ */

#define IPC_MSG_MAX  64
#define IPC_MSG_LEN  128

typedef struct {
    int  from_pid;
    int  to_pid;             /* 0 = broadcast                      */
    char message[IPC_MSG_LEN];
} IpcMessage;

/* Shared memory region */
typedef struct {
    IpcMessage  msgs[IPC_MSG_MAX];
    int         head;
    int         tail;
    int         count;
    pthread_mutex_t lock;
} SharedMemory;

static SharedMemory g_shm = { .head = 0, .tail = 0, .count = 0,
                               .lock = PTHREAD_MUTEX_INITIALIZER };

/*
 * ipc_send()
 * Writes a message into the shared memory ring buffer.
 * Simulates a write system call in kernel mode.
 */
void ipc_send(int from_pid, int to_pid, const char *msg) {
    pthread_mutex_lock(&g_shm.lock);
    if (g_shm.count < IPC_MSG_MAX) {
        IpcMessage *m = &g_shm.msgs[g_shm.tail];
        m->from_pid = from_pid;
        m->to_pid   = to_pid;
        strncpy(m->message, msg, IPC_MSG_LEN - 1);
        g_shm.tail  = (g_shm.tail + 1) % IPC_MSG_MAX;
        g_shm.count++;
    }
    pthread_mutex_unlock(&g_shm.lock);
}

/*
 * ipc_receive()
 * Reads the oldest message for to_pid (or broadcast) from shm.
 * Returns 1 if a message was found, 0 otherwise.
 */
int ipc_receive(int to_pid, IpcMessage *out) {
    pthread_mutex_lock(&g_shm.lock);
    for (int i = 0; i < g_shm.count; i++) {
        int idx = (g_shm.head + i) % IPC_MSG_MAX;
        IpcMessage *m = &g_shm.msgs[idx];
        if (m->to_pid == to_pid || m->to_pid == 0) {
            *out = *m;
            /* Remove by shifting head if it is the first element */
            if (idx == g_shm.head) {
                g_shm.head  = (g_shm.head + 1) % IPC_MSG_MAX;
                g_shm.count--;
            }
            pthread_mutex_unlock(&g_shm.lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_shm.lock);
    return 0;
}

/*
 * ipc_display_messages()
 * Kernel-mode function: dumps all pending IPC messages.
 */
void ipc_display_messages(void) {
    pthread_mutex_lock(&g_shm.lock);
    printf("\n--- IPC Shared Memory Messages (%d pending) ---\n", g_shm.count);
    if (g_shm.count == 0) {
        printf("  (no messages)\n");
    } else {
        for (int i = 0; i < g_shm.count; i++) {
            int idx = (g_shm.head + i) % IPC_MSG_MAX;
            IpcMessage *m = &g_shm.msgs[idx];
            printf("  [%d] From PID=%-4d  To PID=%-4s  Msg: %s\n",
                   i+1, m->from_pid,
                   m->to_pid == 0 ? "ALL" : (char[]){
                       (char)('0' + m->to_pid/10),
                       (char)('0' + m->to_pid%10), '\0'},
                   m->message);
        }
    }
    printf("-----------------------------------------------\n");
    pthread_mutex_unlock(&g_shm.lock);
}

/* ============================================================
 * PRIORITY SCHEDULING HELPER
 * ============================================================ */

/*
 * rq_dequeue_by_priority()
 * Bonus: Scans the ready queue and returns the PID of the
 * process with the highest priority (lowest priority number).
 * Falls back to FCFS order if priorities are equal.
 * Returns -1 if queue is empty.
 */
int rq_dequeue_by_priority(void) {
    pthread_mutex_lock(&g_rq_mtx);
    if (g_ready_queue.count == 0) {
        pthread_mutex_unlock(&g_rq_mtx);
        return -1;
    }

    int best_pos = -1;
    int best_pri = 9999;
    for (int i = 0; i < g_ready_queue.count; i++) {
        int idx = (g_ready_queue.head + i) % MAX_PROCESSES;
        int pid = g_ready_queue.pids[idx];
        /* Look up priority in process table */
        for (int j = 0; j < MAX_PROCESSES; j++) {
            if (g_proc_table[j].active && g_proc_table[j].pid == pid) {
                if (g_proc_table[j].priority < best_pri) {
                    best_pri = g_proc_table[j].priority;
                    best_pos = i;
                }
                break;
            }
        }
    }

    /* Extract that entry from the circular queue */
    int best_idx = (g_ready_queue.head + best_pos) % MAX_PROCESSES;
    int chosen_pid = g_ready_queue.pids[best_idx];

    /* Shift remaining entries to fill the gap */
    for (int i = best_pos; i < g_ready_queue.count - 1; i++) {
        int cur  = (g_ready_queue.head + i)     % MAX_PROCESSES;
        int next = (g_ready_queue.head + i + 1) % MAX_PROCESSES;
        g_ready_queue.pids[cur] = g_ready_queue.pids[next];
    }
    g_ready_queue.tail  = (g_ready_queue.tail - 1 + MAX_PROCESSES) % MAX_PROCESSES;
    g_ready_queue.count--;

    pthread_mutex_unlock(&g_rq_mtx);
    return chosen_pid;
}


/* ============================================================
 * SEAT DATABASE PERSISTENCE
 * ============================================================ */

/*
 * init_seats()
 * Loads seat state from seats.txt if it exists,
 * otherwise creates a fresh 50-seat database.
 */
void init_seats(void) {
    FILE *f = fopen(SEAT_FILE, "r");
    int loaded = 0;
    if (f) {
        char line[256];
        int idx = 0;
        while (fgets(line, sizeof(line), f) && idx < MAX_SEATS) {
            Seat *s = &g_seats[idx];
            int booked;
            if (sscanf(line, "%d|%d|%63[^|]|%31[^|]|%31[^|]|%15[^|]|%15[^|]|%15[^\n]",
                    &s->seat_number, &booked,
                    s->passenger_name, s->passenger_id,
                    s->booking_id, s->train_number,
                    s->journey_date, s->seat_class) >= 7) {
                s->is_booked = booked;
                idx++;
            }
        }
        fclose(f);
        if (idx == MAX_SEATS) loaded = 1;
    }

    if (!loaded) {
        for (int i = 0; i < MAX_SEATS; i++) {
            g_seats[i].seat_number = i + 1;
            g_seats[i].is_booked   = 0;
            memset(g_seats[i].passenger_name, 0, MAX_NAME_LEN);
            memset(g_seats[i].passenger_id,   0, MAX_ID_LEN);
            memset(g_seats[i].booking_id,     0, MAX_ID_LEN);
            strncpy(g_seats[i].train_number,  "TR-101", 15);
            strncpy(g_seats[i].journey_date,  "2025-07-01", MAX_DATE_LEN - 1);
            if      (i < 10) strncpy(g_seats[i].seat_class, "First",    15);
            else if (i < 25) strncpy(g_seats[i].seat_class, "Business", 15);
            else             strncpy(g_seats[i].seat_class, "Economy",  15);
        }
    }
    printf("[BOOT] Seat database initialized. Total seats: %d\n", MAX_SEATS);
}

/*
 * save_seats()
 * Writes current seat state to seats.txt for persistence.
 */
void save_seats(void) {
    FILE *f = fopen(SEAT_FILE, "w");
    if (!f) return;
    for (int i = 0; i < MAX_SEATS; i++) {
        Seat *s = &g_seats[i];
        fprintf(f, "%d|%d|%s|%s|%s|%s|%s|%s\n",
                s->seat_number, s->is_booked,
                s->passenger_name, s->passenger_id,
                s->booking_id, s->train_number,
                s->journey_date, s->seat_class);
    }
    fclose(f);
}

/*
 * load_bookings()
 * Reads bookings.txt into g_bookings[] on startup.
 */
void load_bookings(void) {
    FILE *f = fopen(BOOKING_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && g_booking_count < MAX_PROCESSES * 4) {
        BookingRecord *r = &g_bookings[g_booking_count];
        int seat;
        if (sscanf(line, "%31[^|]|%63[^|]|%31[^|]|%d|%15[^|]|%15[^|]|%15[^|]|%15[^|]|%31[^\n]",
                r->booking_id, r->passenger_name, r->passenger_id,
                &seat, r->train_number, r->journey_date,
                r->seat_class, r->status, r->timestamp) >= 8) {
            r->seat_number = seat;
            g_booking_count++;
        }
    }
    fclose(f);
    printf("[BOOT] Loaded %d booking records from storage.\n", g_booking_count);
}

/*
 * save_bookings()
 * Writes all booking records to bookings.txt.
 */
void save_bookings(void) {
    FILE *f = fopen(BOOKING_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_booking_count; i++) {
        BookingRecord *r = &g_bookings[i];
        fprintf(f, "%s|%s|%s|%d|%s|%s|%s|%s|%s\n",
                r->booking_id, r->passenger_name, r->passenger_id,
                r->seat_number, r->train_number, r->journey_date,
                r->seat_class, r->status, r->timestamp);
    }
    fclose(f);
}

/* ============================================================
 * BOOT AND SHUTDOWN
 * ============================================================ */

/*
 * boot_system()
 * Full OS boot: POST simulation, kernel load, resource config,
 * seat DB init, scheduling algo selection.
 */
void boot_system(int ram_gb, int disk_gb, int cpu_cores) {
    clear_screen();
    print_line('=', 65);
    printf("      MINI OPERATING SYSTEM SIMULATOR v1.0\n");
    printf("      Concurrent Railway Reservation System\n");
    printf("      CL-2006 Operating Systems Lab - Final Project\n");
    print_line('=', 65);
    printf("\n");

    /* POST */
    printf("[BOOT] Power On Self Test (POST)...\n");
    usleep(300000);
    printf("  [ OK ] CPU check..................... PASS\n"); usleep(200000);
    printf("  [ OK ] Memory check.................. PASS\n"); usleep(200000);
    printf("  [ OK ] Storage check................. PASS\n"); usleep(200000);
    printf("  [ OK ] Interrupt controller.......... PASS\n"); usleep(200000);
    printf("  [ OK ] Semaphore/Mutex init.......... PASS\n"); usleep(200000);
    printf("[BOOT] POST complete.\n\n");

    /* Kernel load */
    printf("[BOOT] Loading Kernel Components...\n");
    usleep(200000);
    printf("  Process Manager.................. OK\n"); usleep(150000);
    printf("  CPU Scheduler.................... OK\n"); usleep(150000);
    printf("  Memory Manager................... OK\n"); usleep(150000);
    printf("  IPC Module....................... OK\n"); usleep(150000);
    printf("  File System...................... OK\n"); usleep(150000);
    printf("  Interrupt Handler................ OK\n"); usleep(150000);
    printf("[BOOT] Kernel loaded.\n\n");

    /* Resource init */
    g_resources.total_ram_mb       = (long)ram_gb  * 1024;
    g_resources.available_ram_mb   = g_resources.total_ram_mb;
    g_resources.total_disk_gb      = disk_gb;
    g_resources.available_disk_gb  = disk_gb;
    g_resources.total_cores        = cpu_cores > 8 ? 8 : cpu_cores;
    g_resources.available_cores    = g_resources.total_cores;
    for (int i = 0; i < 8; i++) g_resources.core_busy[i] = 0;

    printf("[BOOT] Resources configured:\n");
    printf("       RAM   : %d GB\n", ram_gb);
    printf("       Disk  : %d GB\n", disk_gb);
    printf("       Cores : %d\n\n", g_resources.total_cores);

    /* Semaphore: binary, 1 thread in seat critical section at a time */
    sem_init(&g_seat_sem, 0, 1);

    /* Ready queue init */
    memset(&g_ready_queue, 0, sizeof(ReadyQueue));

    /* Seat DB and bookings */
    init_seats();
    load_bookings();

    /* Scheduler selection */
    printf("\n[BOOT] Select CPU Scheduling Algorithm:\n");
    printf("       1. First Come First Served (FCFS)\n");
    printf("       2. Round Robin\n");
    printf("       3. Priority Scheduling (Bonus)\n");
    printf("  Choice [1/2/3]: ");
    int choice = 1;
    scanf("%d", &choice); getchar();
    g_sched_algo = (choice == 2) ? ALGO_ROUND_ROBIN
                 : (choice == 3) ? ALGO_PRIORITY
                 :                 ALGO_FCFS;

    if (g_sched_algo == ALGO_ROUND_ROBIN) {
        printf("  Time Quantum (seconds) [default=%d]: ", RR_QUANTUM_DEF);
        int q = 0;
        scanf("%d", &q); getchar();
        if (q > 0) g_rr_quantum = q;
    }

    g_system_running = 1;

    printf("\n[BOOT] Scheduler : %s\n",
           g_sched_algo == ALGO_FCFS        ? "First Come First Served"
         : g_sched_algo == ALGO_ROUND_ROBIN ? "Round Robin"
         :                                    "Priority Scheduling");
    printf("[BOOT] System boot complete. Mini OS is RUNNING.\n");
    print_line('-', 65);

    log_event("INFO", "System booted successfully.");
}

/*
 * shutdown_system()
 * Saves all data, destroys semaphore, terminates all processes,
 * logs shutdown.
 */
void shutdown_system(void) {
    printf("\n[SHUTDOWN] Initiating graceful shutdown...\n");
    g_shutdown_req   = 1;
    g_system_running = 0;

    /* Trigger SHUTDOWN interrupt on all running processes */
    pthread_mutex_lock(&g_proc_mtx);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc_table[i].active &&
            g_proc_table[i].state != STATE_TERMINATED) {
            g_proc_table[i].state = STATE_TERMINATED;
        }
    }
    pthread_mutex_unlock(&g_proc_mtx);

    save_bookings();
    save_seats();

    sem_destroy(&g_seat_sem);

    printf("[SHUTDOWN] Bookings saved to %s\n", BOOKING_FILE);
    printf("[SHUTDOWN] Seat state saved to %s\n", SEAT_FILE);
    log_event("INFO", "System shutdown complete.");
    printf("[SHUTDOWN] System halted. Goodbye.\n");
    print_line('=', 65);
}

/* ============================================================
 * RAILWAY RESERVATION SYSTEM FUNCTIONS
 * ============================================================
 * Each function is run inside a dedicated POSIX thread that
 * represents a passenger process. Mutexes and semaphores
 * protect the shared seat database (critical sections).
 * ============================================================ */

/*
 * find_booking_by_id()
 * Searches g_bookings for a matching booking_id.
 * Returns index or -1 if not found.
 */
int find_booking_by_id(const char *bid) {
    for (int i = 0; i < g_booking_count; i++)
        if (strcmp(g_bookings[i].booking_id, bid) == 0) return i;
    return -1;
}

/*
 * book_ticket()
 * CRITICAL SECTION: Uses semaphore to ensure only one thread
 * modifies seat data at a time. Prevents double-booking.
 * Parameters via pipe-delimited request_data:
 *   passenger_name|passenger_id|seat_class|journey_date
 */
void book_ticket(int pid, const char *req_data, const char *passenger_name) {
    char pname[MAX_NAME_LEN], pid_str[MAX_ID_LEN],
         seat_class[16], jdate[MAX_DATE_LEN];
    memset(pname,      0, sizeof(pname));
    memset(pid_str,    0, sizeof(pid_str));
    memset(seat_class, 0, sizeof(seat_class));
    memset(jdate,      0, sizeof(jdate));

    sscanf(req_data, "%63[^|]|%31[^|]|%15[^|]|%15[^\n]",
           pname, pid_str, seat_class, jdate);

    printf("[PID=%-4d] %s requesting BOOK | Class=%s | Date=%s\n",
           pid, pname[0] ? pname : passenger_name, seat_class, jdate);

    /* ── ENTER CRITICAL SECTION ── */
    printf("[PID=%-4d] Waiting for seat semaphore...\n", pid);
    sem_wait(&g_seat_sem);
    pthread_mutex_lock(&g_seat_mutex);
    printf("[PID=%-4d] Entered critical section.\n", pid);

    /* Find an available seat of requested class */
    int found = -1;
    for (int i = 0; i < MAX_SEATS; i++) {
        if (!g_seats[i].is_booked &&
            (seat_class[0] == '\0' ||
             strcasecmp(g_seats[i].seat_class, seat_class) == 0)) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* Assign seat */
        char bid[MAX_ID_LEN];
        snprintf(bid, sizeof(bid), "BK-%d", g_next_booking++);

        g_seats[found].is_booked = 1;
        strncpy(g_seats[found].passenger_name, pname[0] ? pname : passenger_name, MAX_NAME_LEN - 1);
        strncpy(g_seats[found].passenger_id,   pid_str,  MAX_ID_LEN - 1);
        strncpy(g_seats[found].booking_id,     bid,      MAX_ID_LEN - 1);
        if (jdate[0]) strncpy(g_seats[found].journey_date, jdate, MAX_DATE_LEN - 1);

        /* Add booking record */
        if (g_booking_count < MAX_PROCESSES * 4) {
            BookingRecord *r = &g_bookings[g_booking_count++];
            strncpy(r->booking_id,     bid,                             MAX_ID_LEN   - 1);
            strncpy(r->passenger_name, pname[0] ? pname : passenger_name, MAX_NAME_LEN - 1);
            strncpy(r->passenger_id,   pid_str,                         MAX_ID_LEN   - 1);
            r->seat_number = g_seats[found].seat_number;
            strncpy(r->train_number,   g_seats[found].train_number,     15);
            strncpy(r->journey_date,   g_seats[found].journey_date,     MAX_DATE_LEN - 1);
            strncpy(r->seat_class,     g_seats[found].seat_class,       15);
            strncpy(r->status,         "CONFIRMED",                     15);
            get_timestamp(r->timestamp, sizeof(r->timestamp));
        }

        /* ── EXIT CRITICAL SECTION ── */
        pthread_mutex_unlock(&g_seat_mutex);
        sem_post(&g_seat_sem);
        printf("[PID=%-4d] EXIT critical section.\n", pid);

        printf("[PID=%-4d] BOOKING CONFIRMED\n", pid);
        printf("           Booking ID  : %s\n",   bid);
        printf("           Seat Number : %d\n",   g_seats[found].seat_number);
        printf("           Class       : %s\n",   g_seats[found].seat_class);
        printf("           Train       : %s\n",   g_seats[found].train_number);
        printf("           Date        : %s\n",   g_seats[found].journey_date);

        char logmsg[256];
        snprintf(logmsg, sizeof(logmsg),
                 "PID=%d Booking=%s Seat=%d Passenger=%s",
                 pid, bid, g_seats[found].seat_number,
                 pname[0] ? pname : passenger_name);
        log_event("INFO", logmsg);
        /* IPC: broadcast booking confirmation */
        { char ipc_msg[IPC_MSG_LEN];
          snprintf(ipc_msg, sizeof(ipc_msg), "BOOKING_CONFIRMED Seat=%d BID=%s",
                   g_seats[found].seat_number, bid);
          ipc_send(pid, 0, ipc_msg); }
    } else {
        /* ── EXIT CRITICAL SECTION (no seat) ── */
        pthread_mutex_unlock(&g_seat_mutex);
        sem_post(&g_seat_sem);
        printf("[PID=%-4d] EXIT critical section.\n", pid);

        printf("[PID=%-4d] BOOKING FAILED: No available seat in class '%s'.\n",
               pid, seat_class[0] ? seat_class : "Any");
        log_event("WARN", "Booking failed - no available seat");
    }
}

/*
 * cancel_ticket()
 * CRITICAL SECTION: Cancels an existing booking by booking_id.
 * Frees the seat and updates the booking record status.
 * Triggers a CANCELLATION interrupt event.
 */
void cancel_ticket(int pid, const char *req_data) {
    char bid[MAX_ID_LEN];
    strncpy(bid, req_data, MAX_ID_LEN - 1);
    bid[MAX_ID_LEN - 1] = '\0';
    /* Trim newline */
    for (int i = 0; bid[i]; i++) if (bid[i] == '\n') { bid[i] = '\0'; break; }

    printf("[PID=%-4d] Requesting CANCEL for Booking=%s\n", pid, bid);

    /* Trigger interrupt */
    handle_interrupt(pid, INT_CANCELLATION);

    /* ── ENTER CRITICAL SECTION ── */
    sem_wait(&g_seat_sem);
    pthread_mutex_lock(&g_seat_mutex);

    /* Find seat with this booking_id */
    int seat_idx = -1;
    for (int i = 0; i < MAX_SEATS; i++) {
        if (g_seats[i].is_booked &&
            strcmp(g_seats[i].booking_id, bid) == 0) {
            seat_idx = i;
            break;
        }
    }

    if (seat_idx >= 0) {
        char pname[MAX_NAME_LEN];
        strncpy(pname, g_seats[seat_idx].passenger_name, MAX_NAME_LEN - 1);
        int snum = g_seats[seat_idx].seat_number;

        /* Free the seat */
        g_seats[seat_idx].is_booked = 0;
        memset(g_seats[seat_idx].passenger_name, 0, MAX_NAME_LEN);
        memset(g_seats[seat_idx].passenger_id,   0, MAX_ID_LEN);
        memset(g_seats[seat_idx].booking_id,     0, MAX_ID_LEN);

        /* Update booking record */
        int bidx = find_booking_by_id(bid);
        if (bidx >= 0) strncpy(g_bookings[bidx].status, "CANCELLED", 15);

        pthread_mutex_unlock(&g_seat_mutex);
        sem_post(&g_seat_sem);

        printf("[PID=%-4d] CANCELLATION CONFIRMED\n", pid);
        printf("           Booking ID  : %s\n", bid);
        printf("           Seat Number : %d\n", snum);
        printf("           Passenger   : %s\n", pname);

        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "PID=%d Cancelled Booking=%s Seat=%d", pid, bid, snum);
        log_event("INFO", logmsg);
    } else {
        pthread_mutex_unlock(&g_seat_mutex);
        sem_post(&g_seat_sem);
        printf("[PID=%-4d] CANCEL FAILED: Booking ID '%s' not found.\n", pid, bid);
        log_event("WARN", "Cancellation failed - booking not found");
    }
}

/*
 * view_seats()
 * Displays all seats with their current booking status.
 * Read-only; uses mutex for consistency (no semaphore needed).
 */
void view_seats(int pid) {
    printf("[PID=%-4d] Requesting VIEW AVAILABLE SEATS\n\n", pid);

    pthread_mutex_lock(&g_seat_mutex);

    print_line('-', 70);
    printf("%-6s %-12s %-10s %-20s %-15s\n",
           "Seat", "Class", "Status", "Passenger", "Booking ID");
    print_line('-', 70);

    int available = 0;
    for (int i = 0; i < MAX_SEATS; i++) {
        Seat *s = &g_seats[i];
        printf("%-6d %-12s %-10s %-20s %-15s\n",
               s->seat_number,
               s->seat_class,
               s->is_booked ? "BOOKED" : "AVAILABLE",
               s->is_booked ? s->passenger_name : "-",
               s->is_booked ? s->booking_id     : "-");
        if (!s->is_booked) available++;
    }

    pthread_mutex_unlock(&g_seat_mutex);

    print_line('-', 70);
    printf("Available seats: %d / %d\n", available, MAX_SEATS);
    log_event("INFO", "Seat availability viewed");
}

/*
 * view_booking_status()
 * Shows details of a specific booking by booking_id.
 */
void view_booking_status(int pid, const char *bid) {
    printf("[PID=%-4d] Requesting STATUS for Booking=%s\n", pid, bid);

    pthread_mutex_lock(&g_seat_mutex);
    int idx = find_booking_by_id(bid);

    if (idx >= 0) {
        BookingRecord *r = &g_bookings[idx];
        print_line('-', 50);
        printf("Booking Details\n");
        print_line('-', 50);
        printf("Booking ID    : %s\n", r->booking_id);
        printf("Passenger     : %s\n", r->passenger_name);
        printf("Passenger ID  : %s\n", r->passenger_id);
        printf("Seat Number   : %d\n", r->seat_number);
        printf("Train         : %s\n", r->train_number);
        printf("Date          : %s\n", r->journey_date);
        printf("Class         : %s\n", r->seat_class);
        printf("Status        : %s\n", r->status);
        printf("Booked At     : %s\n", r->timestamp);
        print_line('-', 50);
    } else {
        printf("Booking ID '%s' not found.\n", bid);
    }

    pthread_mutex_unlock(&g_seat_mutex);
    log_event("INFO", "Booking status viewed");
}

/*
 * passenger_info()
 * Searches all bookings for a given passenger name and
 * displays their complete reservation history.
 */
void passenger_info(int pid, const char *pname) {
    printf("[PID=%-4d] Requesting PASSENGER INFO for: %s\n", pid, pname);

    pthread_mutex_lock(&g_seat_mutex);
    int found = 0;

    print_line('-', 60);
    printf("Booking History for: %s\n", pname);
    print_line('-', 60);

    for (int i = 0; i < g_booking_count; i++) {
        if (strcasecmp(g_bookings[i].passenger_name, pname) == 0) {
            BookingRecord *r = &g_bookings[i];
            printf("  Booking %-10s  Seat %-4d  Train %-8s  Date %-12s  [%s]\n",
                   r->booking_id, r->seat_number, r->train_number,
                   r->journey_date, r->status);
            found++;
        }
    }

    pthread_mutex_unlock(&g_seat_mutex);

    if (!found) printf("  No bookings found for '%s'.\n", pname);
    print_line('-', 60);
    log_event("INFO", "Passenger info viewed");
}

/* ============================================================
 * PASSENGER THREAD FUNCTION
 * ============================================================ */

/*
 * passenger_thread()
 * Entry point for each passenger thread. Simulates the
 * process running on the CPU:
 *   NEW -> READY (done by create_process)
 *   READY -> RUNNING (done here after scheduling)
 *   RUNNING -> TERMINATED (after request handled)
 * Uses CPU core allocation to enforce resource limits.
 */
void *passenger_thread(void *arg) {
    PassengerArg *pa = (PassengerArg *)arg;
    int pid = pa->pid;

    /* Wait for a CPU core (blocks if all cores busy) */
    int core = -1;
    int wait_count = 0;
    while (core < 0 && !g_shutdown_req) {
        core = allocate_core();
        if (core < 0) {
            if (wait_count == 0)
                printf("[PID=%-4d] Waiting for free CPU core...\n", pid);
            wait_count++;
            usleep(200000);  /* wait 200ms then retry */
        }
    }
    if (g_shutdown_req) { free(pa); return NULL; }

    /* Assign core to PCB and move to RUNNING */
    pthread_mutex_lock(&g_proc_mtx);
    PCB *p = find_pcb(pid);
    if (p) { p->cpu_core = core; p->state = STATE_RUNNING; }
    pthread_mutex_unlock(&g_proc_mtx);

    printf("[OS] PID=%-4d assigned to Core %d -> RUNNING\n", pid, core);
    log_proc_event(pid, "RUNNING", STATE_RUNNING);

    /* Simulate burst time for FCFS (run to completion)
       For Round Robin the scheduler would preempt, but since
       our requests are short we simulate the quantum sleep. */
    if (g_sched_algo == ALGO_ROUND_ROBIN) {
        usleep(g_rr_quantum * 100000);   /* simulate quantum */
    } else {
        usleep(300000);                   /* simulate burst   */
    }

    /* Timeout interrupt check (simulated 5% chance for demo) */
    srand((unsigned int)(time(NULL) ^ (pid * 31)));
    if (rand() % 20 == 0) {
        handle_interrupt(pid, INT_TIMEOUT);
        printf("[PID=%-4d] Process timed out.\n", pid);
        free(pa);
        return NULL;
    }

    /* ── Execute the actual request ── */
    if (strcmp(pa->request_type, "BOOK") == 0) {
        book_ticket(pid, pa->request_data, pa->passenger_name);
    } else if (strcmp(pa->request_type, "CANCEL") == 0) {
        cancel_ticket(pid, pa->request_data);
    } else if (strcmp(pa->request_type, "VIEW") == 0) {
        view_seats(pid);
    } else if (strcmp(pa->request_type, "STATUS") == 0) {
        view_booking_status(pid, pa->request_data);
    } else if (strcmp(pa->request_type, "INFO") == 0) {
        passenger_info(pid, pa->request_data);
    }

    /* Process complete -> TERMINATED */
    terminate_process(pid);

    free(pa);
    return NULL;
}

/*
 * spawn_passenger_thread()
 * Creates a PassengerArg, calls create_process(), then
 * spawns a POSIX thread to handle the request concurrently.
 */
void spawn_passenger_thread(const char *pname, const char *req_type,
                             const char *req_data, int priority)
{
    int pid = create_process(pname, req_type, req_data, priority, 1000);
    if (pid < 0) return;

    PassengerArg *arg = malloc(sizeof(PassengerArg));
    if (!arg) { terminate_process(pid); return; }

    arg->pid = pid;
    strncpy(arg->request_type,   req_type, 15);
    strncpy(arg->request_data,   req_data, 255);
    strncpy(arg->passenger_name, pname,    MAX_NAME_LEN - 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, passenger_thread, arg) != 0) {
        printf("[OS] ERROR: Failed to create thread for PID=%d\n", pid);
        terminate_process(pid);
        free(arg);
    } else {
        pthread_detach(tid);  /* no join needed; thread cleans itself */
    }
}

/* ============================================================
 * DISPLAY FUNCTIONS
 * ============================================================ */

/*
 * display_process_table()
 * Prints all PCBs in a formatted table for admin view.
 * Requires KERNEL mode.
 */
void display_process_table(void) {
    pthread_mutex_lock(&g_proc_mtx);
    print_line('-', 88);
    printf("%-6s %-22s %-12s %-8s %-10s %-6s %-8s\n",
           "PID", "Process Name", "State", "Priority", "Burst(ms)", "Core", "RAM(MB)");
    print_line('-', 88);

    int any = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &g_proc_table[i];
        if (!p->active) continue;
        printf("%-6d %-22s %-12s %-8d %-10d %-6s %-8d\n",
               p->pid,
               p->process_name,
               state_name(p->state),
               p->priority,
               p->burst_time,
               p->cpu_core >= 0 ? (char[]){(char)('0'+p->cpu_core),'\0'} : "N/A",
               p->memory_mb);
        any = 1;
    }
    if (!any) printf("  No processes in table.\n");
    pthread_mutex_unlock(&g_proc_mtx);

    print_line('-', 88);
    printf("RAM  : %ld MB free / %ld MB total\n",
           g_resources.available_ram_mb, g_resources.total_ram_mb);
    printf("Cores: %d free / %d total\n",
           g_resources.available_cores, g_resources.total_cores);
}

/*
 * display_ready_queue()
 * Shows which PIDs are currently waiting in the ready queue.
 */
void display_ready_queue(void) {
    pthread_mutex_lock(&g_rq_mtx);
    printf("Ready Queue (%d process(es)):\n", g_ready_queue.count);
    if (g_ready_queue.count == 0) {
        printf("  (empty)\n");
    } else {
        int idx = g_ready_queue.head;
        for (int i = 0; i < g_ready_queue.count; i++) {
            printf("  [%d] PID=%d\n", i + 1, g_ready_queue.pids[idx]);
            idx = (idx + 1) % MAX_PROCESSES;
        }
    }
    pthread_mutex_unlock(&g_rq_mtx);
}

/*
 * display_resources()
 * Shows current system resource utilization.
 */
void display_resources(void) {
    print_line('-', 50);
    printf("SYSTEM RESOURCE STATUS\n");
    print_line('-', 50);
    pthread_mutex_lock(&g_res_mtx);
    printf("RAM      : %ld MB available / %ld MB total\n",
           g_resources.available_ram_mb, g_resources.total_ram_mb);
    printf("Disk     : %ld GB available / %ld GB total\n",
           g_resources.available_disk_gb, g_resources.total_disk_gb);
    printf("CPU Cores: %d available / %d total\n",
           g_resources.available_cores, g_resources.total_cores);
    printf("Core Map : ");
    for (int i = 0; i < g_resources.total_cores; i++)
        printf("Core%d=%s  ", i, g_resources.core_busy[i] ? "BUSY" : "FREE");
    printf("\n");
    pthread_mutex_unlock(&g_res_mtx);
    print_line('-', 50);
}

/*
 * display_all_bookings()
 * Lists all booking records (confirmed and cancelled).
 */
void display_all_bookings(void) {
    pthread_mutex_lock(&g_seat_mutex);
    print_line('-', 80);
    printf("%-14s %-20s %-6s %-10s %-12s %-12s\n",
           "Booking ID", "Passenger", "Seat", "Class", "Date", "Status");
    print_line('-', 80);

    if (g_booking_count == 0) {
        printf("  No booking records found.\n");
    } else {
        for (int i = 0; i < g_booking_count; i++) {
            BookingRecord *r = &g_bookings[i];
            printf("%-14s %-20s %-6d %-10s %-12s %-12s\n",
                   r->booking_id, r->passenger_name,
                   r->seat_number, r->seat_class,
                   r->journey_date, r->status);
        }
    }
    pthread_mutex_unlock(&g_seat_mutex);
    print_line('-', 80);
}

/* ============================================================
 * MENU HELPERS
 * ============================================================ */

/*
 * read_line()
 * Safely reads a line from stdin into buf of size len.
 * Strips trailing newline.
 */
void read_line(char *buf, int len) {
    fgets(buf, len, stdin);
    int l = strlen(buf);
    if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
}

/*
 * demo_concurrent_booking()
 * Spawns multiple passenger threads simultaneously to
 * demonstrate concurrency, race-condition prevention,
 * and synchronization in action.
 */
void demo_concurrent_booking(void) {
    printf("\n[DEMO] Launching 5 concurrent passenger requests...\n\n");

    spawn_passenger_thread("Ali_Hassan",    "BOOK",   "Ali Hassan|P001|Economy|2025-07-01",  3);
    usleep(10000);
    spawn_passenger_thread("Sara_Khan",     "BOOK",   "Sara Khan|P002|Business|2025-07-01",  2);
    usleep(10000);
    spawn_passenger_thread("Usman_Ali",     "BOOK",   "Usman Ali|P003|First|2025-07-01",     1);
    usleep(10000);
    spawn_passenger_thread("Zara_Malik",    "BOOK",   "Zara Malik|P004|Economy|2025-07-01",  3);
    usleep(10000);
    spawn_passenger_thread("Ahmed_Raza",    "VIEW",   "",                                    4);

    printf("\n[DEMO] All threads spawned. Waiting for completion...\n");
    sleep(3);
    printf("[DEMO] Demo complete.\n\n");
}

/* ============================================================
 * USER MODE MENU
 * ============================================================ */

/*
 * user_menu()
 * Main interactive menu for USER MODE. Passengers can:
 *   - Book a ticket
 *   - Cancel a ticket
 *   - View available seats
 *   - Check booking status
 *   - View passenger information
 */
void user_menu(void) {
    int running = 1;
    while (running && g_system_running) {
        printf("\n");
        print_line('=', 55);
        printf("  RAILWAY RESERVATION SYSTEM  [USER MODE]\n");
        print_line('=', 55);
        printf("  1. Book Ticket\n");
        printf("  2. Cancel Ticket\n");
        printf("  3. View Available Seats\n");
        printf("  4. Check Booking Status\n");
        printf("  5. Passenger Information\n");
        printf("  6. Run Concurrent Demo (5 users)\n");
        printf("  0. Back to Main Menu\n");
        print_line('-', 55);
        printf("  Choice: ");

        int ch = 0;
        scanf("%d", &ch); getchar();

        char pname[MAX_NAME_LEN], pid_str[MAX_ID_LEN];
        char seat_class[16], jdate[MAX_DATE_LEN];
        char req_data[256], bid[MAX_ID_LEN];

        switch (ch) {
            case 1:
                printf("  Passenger Name  : "); read_line(pname,      MAX_NAME_LEN);
                printf("  Passenger ID    : "); read_line(pid_str,    MAX_ID_LEN);
                printf("  Seat Class (Economy/Business/First): ");
                                                read_line(seat_class, 15);
                printf("  Journey Date (YYYY-MM-DD): ");
                                                read_line(jdate,      MAX_DATE_LEN);
                snprintf(req_data, sizeof(req_data), "%s|%s|%s|%s",
                         pname, pid_str, seat_class, jdate);
                spawn_passenger_thread(pname, "BOOK", req_data, 3);
                sleep(2);  /* wait for thread to finish before next prompt */
                break;

            case 2:
                printf("  Booking ID to cancel: "); read_line(bid, MAX_ID_LEN);
                spawn_passenger_thread("User", "CANCEL", bid, 2);
                sleep(2);
                break;

            case 3:
                spawn_passenger_thread("Viewer", "VIEW", "", 5);
                sleep(1);
                break;

            case 4:
                printf("  Booking ID: "); read_line(bid, MAX_ID_LEN);
                spawn_passenger_thread("User", "STATUS", bid, 4);
                sleep(1);
                break;

            case 5:
                printf("  Passenger Name: "); read_line(pname, MAX_NAME_LEN);
                spawn_passenger_thread(pname, "INFO", pname, 4);
                sleep(1);
                break;

            case 6:
                demo_concurrent_booking();
                break;

            case 0:
                running = 0;
                break;

            default:
                printf("  Invalid option. Try again.\n");
        }
    }
}

/* ============================================================
 * KERNEL MODE MENU
 * ============================================================ */

/*
 * kernel_menu()
 * Admin menu for KERNEL MODE. Provides privileged operations:
 *   - View and terminate processes
 *   - Manage memory and CPU
 *   - Reset seat database
 *   - View system logs
 *   - Force save data
 */
void kernel_menu(void) {
    int running = 1;
    while (running && g_system_running) {
        printf("\n");
        print_line('=', 55);
        printf("  KERNEL MODE  [Admin / OS Control]\n");
        print_line('=', 55);
        printf("  1.  View Process Table\n");
        printf("  2.  View Ready Queue\n");
        printf("  3.  Terminate a Process (by PID)\n");
        printf("  4.  View System Resources\n");
        printf("  5.  View All Booking Records\n");
        printf("  6.  Reset Seat Database\n");
        printf("  7.  Change Scheduling Algorithm\n");
        printf("  8.  View System Log\n");
        printf("  9.  Force Save All Data\n");
        printf("  10. Simulate Interrupt\n");
        printf("  11. View IPC Shared Memory\n");
        printf("  0.  Back to Main Menu\n");
        print_line('-', 55);
        printf("  Choice: ");

        int ch = 0;
        scanf("%d", &ch); getchar();

        int pid;
        char line[64];

        switch (ch) {
            case 1:
                display_process_table();
                break;

            case 2:
                display_ready_queue();
                break;

            case 3:
                printf("  PID to terminate: ");
                scanf("%d", &pid); getchar();
                terminate_process(pid);
                break;

            case 4:
                display_resources();
                break;

            case 5:
                display_all_bookings();
                break;

            case 6:
                printf("  Reset all seats? (y/n): ");
                read_line(line, sizeof(line));
                if (line[0] == 'y' || line[0] == 'Y') {
                    pthread_mutex_lock(&g_seat_mutex);
                    for (int i = 0; i < MAX_SEATS; i++) {
                        g_seats[i].is_booked = 0;
                        memset(g_seats[i].passenger_name, 0, MAX_NAME_LEN);
                        memset(g_seats[i].passenger_id,   0, MAX_ID_LEN);
                        memset(g_seats[i].booking_id,     0, MAX_ID_LEN);
                    }
                    pthread_mutex_unlock(&g_seat_mutex);
                    printf("  Seat database reset.\n");
                    log_event("WARN", "Seat database reset by admin.");
                }
                break;

            case 7:
                printf("  1=FCFS  2=Round Robin  3=Priority : ");
                scanf("%d", &ch); getchar();
                if (ch == 1) {
                    g_sched_algo = ALGO_FCFS;
                    printf("  Scheduler: FCFS\n");
                } else if (ch == 2) {
                    printf("  Time Quantum (s): ");
                    scanf("%d", &g_rr_quantum); getchar();
                    g_sched_algo = ALGO_ROUND_ROBIN;
                    printf("  Scheduler: Round Robin (quantum=%ds)\n", g_rr_quantum);
                } else if (ch == 3) {
                    g_sched_algo = ALGO_PRIORITY;
                    printf("  Scheduler: Priority Scheduling\n");
                }
                log_event("INFO", "Scheduling algorithm changed by admin.");
                break;

            case 8:
                printf("\n--- system_log.txt ---\n");
                {
                    FILE *lf = fopen(LOG_FILE, "r");
                    if (lf) {
                        char buf[256];
                        while (fgets(buf, sizeof(buf), lf))
                            fputs(buf, stdout);
                        fclose(lf);
                    } else {
                        printf("  (log file empty or not found)\n");
                    }
                }
                printf("--- end of log ---\n");
                break;

            case 9:
                save_bookings();
                save_seats();
                printf("  All data saved to disk.\n");
                log_event("INFO", "Manual save triggered by admin.");
                break;

            case 11:
                ipc_display_messages();
                break;

            case 10:
                printf("  PID to interrupt: ");
                scanf("%d", &pid); getchar();
                printf("  Interrupt type:\n");
                printf("    1=CANCELLATION  2=TIMEOUT  3=SHUTDOWN\n");
                printf("  Choice: ");
                int itype = 1;
                scanf("%d", &itype); getchar();
                InterruptType it = (itype == 2) ? INT_TIMEOUT
                                 : (itype == 3) ? INT_SHUTDOWN
                                 :                INT_CANCELLATION;
                handle_interrupt(pid, it);
                break;

            case 0:
                running = 0;
                break;

            default:
                printf("  Invalid option.\n");
        }
    }
}

/* ============================================================
 * MAIN MENU
 * ============================================================ */

/*
 * main_menu()
 * Top-level menu. Allows switching between User and Kernel mode
 * and initiating system shutdown.
 */
void main_menu(void) {
    while (g_system_running) {
        printf("\n");
        print_line('=', 55);
        printf("  MINI OS SIMULATOR  |  Mode: %s\n",
               g_mode == MODE_KERNEL ? "KERNEL" : "USER");
        print_line('=', 55);
        printf("  1. Railway Reservation System (User Mode)\n");
        printf("  2. OS Control Panel (Kernel Mode)\n");
        printf("  3. Switch Mode (current: %s)\n",
               g_mode == MODE_USER ? "USER" : "KERNEL");
        printf("  4. System Resource Status\n");
        printf("  0. Shutdown System\n");
        print_line('-', 55);
        printf("  Choice: ");

        int ch = 0;
        scanf("%d", &ch); getchar();

        switch (ch) {
            case 1:
                g_mode = MODE_USER;
                user_menu();
                break;

            case 2:
                if (g_mode != MODE_KERNEL) {
                    char pwd[64];
                    printf("  Enter kernel password: ");
                    read_line(pwd, sizeof(pwd));
                    if (strcmp(pwd, "admin123") != 0) {
                        printf("  Access denied. Incorrect password.\n");
                        break;
                    }
                    g_mode = MODE_KERNEL;
                    printf("  Kernel mode activated.\n");
                    log_event("WARN", "Kernel mode entered.");
                }
                kernel_menu();
                break;

            case 3:
                if (g_mode == MODE_USER) {
                    char pwd[64];
                    printf("  Kernel password to elevate: ");
                    read_line(pwd, sizeof(pwd));
                    if (strcmp(pwd, "admin123") == 0) {
                        g_mode = MODE_KERNEL;
                        printf("  Switched to KERNEL mode.\n");
                        log_event("INFO", "Mode switched to KERNEL.");
                    } else {
                        printf("  Wrong password. Staying in USER mode.\n");
                    }
                } else {
                    g_mode = MODE_USER;
                    printf("  Switched to USER mode.\n");
                    log_event("INFO", "Mode switched to USER.");
                }
                break;

            case 4:
                display_resources();
                break;

            case 0:
                shutdown_system();
                return;

            default:
                printf("  Invalid option.\n");
        }
    }
}

/* ============================================================
 * SIGNAL HANDLER
 * ============================================================ */

/*
 * signal_handler()
 * Catches SIGINT (Ctrl+C) to ensure graceful shutdown
 * and data persistence even on forced exit.
 */
void signal_handler(int sig) {
    (void)sig;
    printf("\n[SIGNAL] SIGINT received. Shutting down gracefully...\n");
    shutdown_system();
    exit(0);
}

/* ============================================================
 * MAIN ENTRY POINT
 * ============================================================ */

/*
 * main()
 * Program entry: reads hardware config from user, boots the OS,
 * then launches the main interactive menu loop.
 */
int main(void) {
    /* Register signal handler for clean shutdown on Ctrl+C */
    signal(SIGINT, signal_handler);

    /* Zero out process table */
    memset(g_proc_table, 0, sizeof(g_proc_table));
    memset(g_bookings,   0, sizeof(g_bookings));
    memset(g_seats,      0, sizeof(g_seats));

    printf("\n");
    print_line('=', 65);
    printf("  MINI OS SIMULATOR - Hardware Configuration\n");
    print_line('=', 65);

    int ram_gb   = 2;
    int disk_gb  = 256;
    int cpu_cores = 8;

    printf("  RAM (GB)   [default=2]  : ");
    scanf("%d", &ram_gb);   if (ram_gb   <= 0) ram_gb   = 2;
    printf("  Disk (GB)  [default=256]: ");
    scanf("%d", &disk_gb);  if (disk_gb  <= 0) disk_gb  = 256;
    printf("  CPU Cores  [default=8]  : ");
    scanf("%d", &cpu_cores);if (cpu_cores <= 0 || cpu_cores > 8) cpu_cores = 8;
    getchar();

    /* Boot the system */
    boot_system(ram_gb, disk_gb, cpu_cores);

    /* Launch interactive menu */
    main_menu();

    return 0;
}
