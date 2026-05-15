#  Mini OS Simulator — Concurrent Railway Reservation System

> **Operating Systems Lab — Final Project**
> A software-level simulation of core Operating System concepts built in **C (POSIX)**, featuring a fully concurrent Railway Reservation System as its primary application.
##  Table of Contents

- [Project Overview](#-project-overview)
- [OS Concepts Implemented](#-os-concepts-implemented)
- [System Architecture](#-system-architecture)
- [Features](#-features)
- [File Structure](#-file-structure)
- [Requirements](#-requirements)
- [Compilation & Running](#-compilation--running)
- [Usage Guide](#-usage-guide)
- [Default Credentials](#-default-credentials)
- [Data Files](#-data-files)


---

##  Project Overview

This project implements a **Mini Operating System Simulator** that models essential OS functionalities such as process management, CPU scheduling, synchronization, and resource allocation. The system includes a **Concurrent Railway Reservation System** as its core application, where multiple users (passengers) access shared resources simultaneously through POSIX threads.

This is a **software-level simulation** — it does not require a real bootable OS.

---

## OS Concepts Implemented

| Concept | Implementation |
|---|---|
| **Boot Simulation** | POST → Kernel load → Resource init sequence |
| **Process Creation & Termination** | PCB (Process Control Block) with full lifecycle |
| **Process States** | NEW → READY → RUNNING → BLOCKED → TERMINATED |
| **Multithreading** | Each passenger request runs as a `pthread` |
| **Concurrency** | Multiple threads access shared seat DB simultaneously |
| **Critical Section** | Seat booking/cancellation protected by mutex + semaphore |
| **Mutex** | `pthread_mutex_t` guards seat DB, process table, log, ready queue |
| **Semaphore** | Binary `sem_t` enforces single-thread access in critical section |
| **CPU Scheduling — FCFS** | First Come First Served via circular ready queue |
| **CPU Scheduling — Round Robin** | Configurable time quantum, preemptive simulation |
| **Ready Queue** | Circular array-based queue with enqueue/dequeue operations |
| **Resource Allocation** | RAM and CPU core tracking with allocation/release |
| **User Mode vs Kernel Mode** | Password-protected kernel panel with elevated privileges |
| **Interrupt Handling** | CANCELLATION, TIMEOUT, SHUTDOWN interrupts with state transition |
| **IPC (Shared Memory)** | Shared seat array accessed across threads (simulated shared memory) |
| **File-based Storage** | Bookings and seat state persisted to disk |
| **System Logging** | All events logged to `system_log.txt` with timestamps |

---

##  System Architecture
System Start
│
├── Boot Simulation
│   ├── POST (Power On Self Test)
│   ├── Kernel Component Loading
│   └── Resource Initialization (RAM / Disk / CPU)
│
├── Mini OS Core
│   ├── Process Manager
│   │   ├── PCB Table
│   │   ├── Process States (NEW → READY → RUNNING → BLOCKED → TERMINATED)
│   │   └── Ready Queue (FCFS / Round Robin)
│   │
│   ├── CPU Scheduler
│   │   ├── First Come First Served (FCFS)
│   │   └── Round Robin (configurable quantum)
│   │
│   ├── Synchronization
│   │   ├── pthread_mutex_t  (seat, process, log, queue, resource)
│   │   └── sem_t            (binary semaphore — seat critical section)
│   │
│   ├── Interrupt Handler
│   │   ├── TICKET_CANCELLATION
│   │   ├── BOOKING_TIMEOUT
│   │   └── SYSTEM_SHUTDOWN
│   │
│   └── User / Kernel Mode
│       ├── User   — booking, cancellation, view
│       └── Kernel — terminate PID, reset DB, change scheduler, view logs
│
└── Railway Reservation System
    ├── Book Ticket       (critical section via mutex + semaphore)
    ├── Cancel Ticket     (interrupt + critical section)
    ├── View Seats        (read with mutex)
    ├── Booking Status    (lookup by booking ID)
    ├── Passenger Info    (search booking history)
    └── File Storage      (bookings.txt, seats.txt, system_log.txt)
##  Features

### Railway Reservation
- Book tickets in **Economy**, **Business**, or **First** class
- Cancel existing bookings by Booking ID
- View real-time seat availability (50 seats total)
- Look up booking status by Booking ID
- Search full booking history by passenger name
- **No double-booking** — enforced via binary semaphore in critical section

### OS Simulation
- Animated boot sequence (POST + kernel load)
- Configurable hardware at startup (RAM, Disk, CPU cores)
- Full PCB lifecycle tracked per passenger thread
- FCFS and Round Robin scheduling with selectable quantum
- CPU core allocation (up to 8 cores) per thread
- RAM allocation checked before every process creation
- Interrupt simulation with process state transitions
- Concurrent demo that spawns 5 threads simultaneously

### Admin (Kernel Mode)
- View process table with states and resource usage
- View ready queue contents
- Forcefully terminate any process by PID
- Reset the entire seat database
- Switch scheduling algorithm at runtime
- Tail system log from within the app
- Manually trigger interrupts on any PID
- Force-save all data to disk

---

##  File Structure

```
railway_os/
│
├── main.c           ← Complete source code (single file)
├── bookings.txt     ← Auto-generated: persistent booking records
├── seats.txt        ← Auto-generated: persistent seat state
├── system_log.txt   ← Auto-generated: timestamped event log
└── README.md        ← This file

##  Requirements

| Requirement | Detail |
|---|---|
| OS | Linux / macOS (any POSIX-compliant system) |
| Compiler | GCC 7+ or Clang |
| Libraries | `pthread`, `rt` (real-time, for semaphores on Linux) |
| C Standard | C99 or later |

> **Windows users:** Use WSL (Windows Subsystem for Linux) or MinGW-w64.

##  Compilation & Running

### Compile

```bash
gcc main.c -o railway_os -lpthread -lrt
```

> On macOS, `-lrt` is not needed:
> ```bash
> gcc main.c -o railway_os -lpthread
> ```

### Run

```bash
./railway_os
```

### Clean generated files (reset state)

```bash
rm -f bookings.txt seats.txt system_log.txt

##  Usage Guide

### Step 1 — Hardware Configuration
On startup, enter your simulated hardware specs (or press Enter to use defaults):
```
RAM (GB)   [default=2]  : 2
Disk (GB)  [default=256]: 256
CPU Cores  [default=8]  : 8
```

### Step 2 — Select Scheduler
```
1. First Come First Served (FCFS)
2. Round Robin
```
If Round Robin is selected, you will be prompted for a time quantum (in seconds).

### Step 3 — Main Menu
```
1. Railway Reservation System (User Mode)
2. OS Control Panel (Kernel Mode)
3. Switch Mode
4. System Resource Status
0. Shutdown System
```

### Step 4 — Book a Ticket (User Mode → Option 1)
```
Passenger Name  : XYZ
Passenger ID    : P001
Seat Class      : Economy
Journey Date    : 2025-07-01
```
A new process (thread) is created, assigned a CPU core, enters the critical section, and books the seat. The booking ID is printed on success.

### Step 5 — Run Concurrent Demo (User Mode → Option 6)
Spawns 5 passenger threads simultaneously to demonstrate race-condition-free concurrent booking using synchronization primitives.

### Step 6 — Kernel Mode (Option 2, password: `admin123`)
Access the OS control panel to manage processes, resources, and system state.

---

##  Default Credentials

| Mode | Password |
|---|---|
| Kernel (Admin) | `admin123` |
| User | *(no password)* |

---

##  Data Files

| File | Description |
|---|---|
| `bookings.txt` | All booking records (pipe-delimited). Loaded on boot, saved on shutdown. |
| `seats.txt` | Current state of all 50 seats. Reloaded on next boot for persistence. |
| `system_log.txt` | Timestamped log of all process events, interrupts, errors, and admin actions. |

**Booking record format:**
```
BK-1001|XYZ |P001|5|TR-101|2025-07-01|Economy|CONFIRMED|2025-06-15 14:32:01

# Notes

- The project is a **simulation** — it does not create real OS processes using `fork()`, but uses POSIX threads (`pthread`) to model concurrent processes sharing resources.
- Seat data and bookings **persist across runs** via text files, simulating secondary storage.
- The binary semaphore (`sem_t`) ensures **only one thread enters the seat critical section** at a time, preventing race conditions and double-booking.
- Interrupts cause a **BLOCKED state transition** and free the CPU core for another process, demonstrating realistic preemption.
]
