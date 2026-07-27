# AURIX TC2x Learning Projects

A hands-on learning repository for the **Infineon AURIX TC2x** microcontroller family (specifically the **TC29B**), using the Infineon Low Level Drivers (iLLD) library.

---

## Hardware

| Item | Details |
|---|---|
| MCU | Infineon AURIX TC29B (TriCore architecture) |
| Target Board | TriBoard TC2X7 V1.0 |
| User LEDs | 8 × active-low LEDs on PORT 33, pins 6–13 |
| Crystal | 20 MHz external oscillator |
| CPU/STM Clock | 200 MHz (configured via PLL) |

---

## Projects

### Test2 — LED Blink (Multicore)

A bare-metal multicore blink example that exercises the three on-chip TriCore CPUs.

**What it does**

- **CPU0** configures PORT 33 pins 6–13 as push-pull outputs and toggles all 8 LEDs every ~500 ms (at 100 MHz STM) or ~250 ms (at 200 MHz STM) using the System Timer Module (STM).
- **CPU1 / CPU2** perform watchdog disable and participate in the CPU synchronisation barrier, then idle in their main loops.
- All cores synchronise at startup via `IfxCpu_syncEvent` before entering their main loops.

**Key concepts demonstrated**

- Multicore startup and CPU synchronisation (`IfxCpu_syncEvent`)
- Watchdog disable (CPU + Safety watchdog)
- GPIO output configuration and toggling via iLLD (`IfxPort_*`)
- Busy-wait delay via STM (`IfxStm_waitTicks`)
- PLL / clock configuration (`Ifx_Cfg.h`)

---

## Repository Structure

```
Tc2x/
└── Test2/                      # Project sources
    ├── Cpu0_Main.c             # Core 0 entry point – LED blink logic
    ├── Cpu1_Main.c             # Core 1 entry point – idle
    ├── Cpu2_Main.c             # Core 2 entry point – idle
    ├── Configurations/
    │   └── Ifx_Cfg.h           # Clock / PLL configuration (20 MHz XTAL, 200 MHz PLL)
    ├── Libraries/
    │   ├── iLLD/TC29B/         # Infineon Low Level Drivers (iLLD)
    │   ├── Infra/              # Platform infrastructure (compiler abstraction, SFR headers)
    │   └── Service/            # Service layer (StdIf, BSP, Math utilities)
    ├── TriCore Debug (GCC)/    # GCC build output
    ├── TriCore Debug (TASKING)/# TASKING build output
    ├── Lcf_Gnuc_Tricore_Tc.lsl     # GCC linker script
    ├── Lcf_Tasking_Tricore_Tc.lsl  # TASKING linker script
    └── winIDEA/                # iSYSTEM winIDEA debug workspace
```

---

## Toolchains

Two compiler toolchains are supported and both have pre-configured launch files:

| Toolchain | Launch file |
|---|---|
| GCC (TriCore) | `test2 TriCore Debug (GCC).launch` |
| TASKING VX-toolset | `test2 TriCore Debug (TASKING)_1.launch` |

Build artefacts (object files, map files, makefiles) are generated under the matching `TriCore Debug (GCC)/` or `TriCore Debug (TASKING)/` folders.

---

## Getting Started

1. **Clone / open** the workspace in your IDE (Eclipse-based AURIX Development Studio or TASKING Eclipse).
2. **Select a build configuration** — `TriCore Debug (GCC)` or `TriCore Debug (TASKING)`.
3. **Build** the project (`Project → Build`).
4. **Flash & debug** using the matching `.launch` configuration or the winIDEA workspace (`winIDEA/workspace.xjrf`).
5. Observe the 8 user LEDs on PORT 33 toggling on the TriBoard TC2X7.

---

## Clock Configuration (`Ifx_Cfg.h`)

| Parameter | Value |
|---|---|
| External oscillator (`XTAL`) | 20 000 000 Hz |
| PLL output frequency | 200 000 000 Hz |

These values are defined in [Test2/Configurations/Ifx_Cfg.h](Test2/Configurations/Ifx_Cfg.h) and consumed by the SCU/PLL driver at startup.

---

## Dependencies

- **iLLD** (Infineon Low Level Drivers) for TC29B — bundled under `Libraries/iLLD/`
- **Infineon SFR headers** for TC29B — bundled under `Libraries/Infra/Sfr/TC29B/`
- **AURIX Development Studio** (Eclipse + GCC) *or* **TASKING VX-toolset** for TriCore

---

## License

Source files carry the **Boost Software License 1.0** as distributed by Infineon Technologies AG.  
See the file headers for the full license text.
