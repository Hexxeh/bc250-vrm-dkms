# AMD BC-250 VRM hwmon driver

Linux kernel `hwmon` driver for reading voltage, current, temperature, and power metrics from the onboard power management IC (PMIC / VRM) on the **AMD BC-250 (Cyan Skillfish)** board.

It reads PMBus Page 0 (CPU VRM) and Page 1 (GPU VRM) over SMBus/I2C via the NCT6687 SuperIO chip and exposes standard, labeled `/sys/class/hwmon/` sensors readable by `lm_sensors` (`sensors`), `CoolerControl`, `MangoHud`, etc.

Earlier versions (<1.1.0) required additional wiring - the latest versions of this module do not.

## Features

Exposes the following sensors and hardware limits in `lm_sensors`:
- **`VIN (12V Input)`**: Main input voltage (mV)
- **`CPU Voltage`**: CPU rail voltage (mV)
- **`GPU Core Voltage`**: GPU rail voltage (mV)
- **`CPU Current`**: CPU rail current (mA) + Over-Current Critical Limit (`curr1_crit`)
- **`GPU Current`**: GPU rail current (mA) + Over-Current Critical Limit (`curr2_crit`)
- **`CPU VRM Temp`**: CPU VRM temperature (°C) + Warning/Critical Limits (`temp1_max`, `temp1_crit`)
- **`GPU VRM Temp`**: GPU VRM temperature (°C) + Warning/Critical Limits (`temp2_max`, `temp2_crit`)
- **`CPU Power`**: Calculated CPU power draw (W)
- **`GPU Power`**: Calculated GPU power draw (W)

---

## Automatic Detection & Loading

The driver includes built-in **DMI board matching** (`AMD BC-250`). When installed, `udev` automatically loads the module on boot and binds to the VRM hardware. This requires a fan driver with correct port reservations as described below; validation of reboot/autoload is reported separately.

---

## DKMS Installation (Auto-Rebuild on Kernel Updates)

### 1. Copy source to `/usr/src/`
```bash
sudo cp -r . /usr/src/bc250-vrm-1.1.0.1
```

### 2. Register & build with DKMS
```bash
sudo dkms add -m bc250-vrm -v 1.1.0.1
sudo dkms build -m bc250-vrm -v 1.1.0.1
sudo dkms install -m bc250-vrm -v 1.1.0.1
```

## Restoration and failure handling

This revision builds on upstream commit
`67aea0d9416eec93ec058af3ff519e81af283950` (version `1.1.0`) and uses
version `1.1.0.1` for both the module and DKMS package. The existing sensor
interface is retained. Manual load/unload/reload and a short workload have been
validated on one P5.00 board; see Board validation below for scope and limits.

The DKMS build follows the target kernel configuration: it enables `LLVM=1`
when `CONFIG_CC_IS_CLANG=y` and otherwise uses the normal kernel build toolchain.
This avoids forcing a Clang build against a GCC-built kernel.

The driver uses EC port 0 on the NCT6686/NCT6687 to read the board's PMBus VRM.
It exposes the original CPU/GPU rail labels, units, current limits and thermal
limits. It does not change voltage targets, current limits, thermal limits or
fan curves. The telemetry path still changes PMBus PAGE and EC SMBus
configuration, so loading it is not a read-only hardware operation.

### Transaction integrity and recovery

- Check the Super-I/O chip, active state and base address before reserving ports.
  Base normalization matches the `nct6687` driver: a low three-bit value of five
  is normalized to the preceding eight-byte boundary; all other unaligned or
  out-of-range addresses are rejected. A second base read must match the first.
  ACPI resource conflicts are checked before EC register writes.
- Save EC CFG1/CFG2 before changes, verify applied values, and restore and verify
  the originals on initialization failure and unload. Refuse configuration
  changes while the controller remains busy.
- Bound the idle wait before each SMBus transfer. After completion or timeout,
  RESET is followed by a bounded idle check. Failure to recover latches an
  error and blocks further transfers. Unexpected changes to the applied CFG
  values also block transfers.
- Lock the Linux I2C bus across PAGE save, select, sensor read and restore.
  The unlocked SMBus API avoids nested bus locking. PAGE selection is verified.
  A failed select still attempts restoration because a failed acknowledgement
  does not prove the device was unchanged. Restoration is confirmed by a
  separate PAGE read even when the restore write reports an error.
- Latch a failed PAGE restoration and preserve its target. Further sensor reads
  return the same error without touching the bus or accepting the changed PAGE
  as a new baseline. Do not enable legacy automatic I2C device probing.
- Fail module initialization if the explicit PMBus client fails to bind.

### Diagnostic contract

The following files are readable under
`/sys/module/bc250_vrm/parameters/`. They have no setter, including at module
load time; they cannot be used to override the safety checks.

| File | Meaning |
| --- | --- |
| `ec_base` | Normalized EC port 0 base address, or -1 before discovery. |
| `saved_cfg1`, `saved_cfg2` | Initial CFG bytes, or -1 before capture. |
| `config_restore_attempted` | 0 before restoration, 1 after an attempt. |
| `config_restore_error` | Restoration errno; 0 means success only when attempted is 1. |
| `transport_error` | Latched controller/configuration fault; 0 when not latched. |
| `initial_page` | First successfully read PAGE, or -1 before a sensor read. |
| `restore_page` | Original PAGE of the failed restoration, or -1. |
| `page_restore_error` | First failed PAGE restoration errno; blocks later sensor reads. |

Before unloading, an operator must preserve these values outside the module.
Kernel messages also record the initial CFG snapshot, restoration attempt and
result, and final PAGE diagnostics. This is necessary when initialization fails
and no sysfs parameters survive. A successful module removal does **not** mean
the hardware was restored. A nonzero restoration error must stop automatic
reload and workload resumption until the original hardware state is verified.
The module does not automatically retry a failed PAGE restoration on removal.
The unchecked `make reload` shortcut is disabled; removal and reload require
explicit verification of these diagnostics. Make and DKMS select Clang only
for kernels configured with `CONFIG_CC_IS_CLANG=y`, and use GCC otherwise.

### Coexistence and remaining limits

The fan-monitoring `nct6687` module must reserve the ports it actually uses.
For EC port 1 this means base+4 through base+7. A driver that reserves port 0
while accessing port 1 must be fixed before loading this driver. The compatible
upstream constants are in
[Fred78290/nct6687d at bac1ed2](https://github.com/Fred78290/nct6687d/blob/bac1ed281aa6a3daaad43597b580a681f2bb85e1/nct6687.c#L194).
Replacing an existing customized fan driver wholesale can lose independent
locking or fan-restoration fixes.

The Linux bus lock excludes other Linux clients of this adapter. It cannot
exclude BIOS/SMM, firmware, another physical bus master or direct port access.
Separate EC PAGE/INDEX ports do not establish that the internal SMBus engine is
exclusively owned. The busy/configuration checks detect some conflicts; they
cannot prove the absence of all firmware races.

Suspend/resume is not supported or validated by this driver. Unload with
confirmed restoration before a suspend attempt. Kernel-version compilation,
manual load, reboot/autoload, fan coexistence and long-duration operation are
separate validation steps. No memory-temperature SMU patch is included.

Telemetry decoding preserves upstream's ISL69247-specific direct formats. It
does not infer generic PMBus LINEAR11 scaling, and PMBUS_REVISION alone does not
prove the chip identity, rail mapping or units. Validate those against known
operating voltage/current/temperature changes on the actual board.

### Host failure tests

`tests/test_bc250_vrm_driver.py` compiles the actual transaction functions and
SMBus implementation against a host stub. It does not use `/dev/port`, PCI,
sysfs hardware writes or a kernel module. The scenarios cover PAGE save/select,
readback and restore failures, fail-stop behavior, bus-lock coverage, busy and
RESET recovery, CFG mismatch and restoration, initialization failures, base
validation and ACPI/resource conflicts. A deliberately broken missing-restore
variant must fail the suite's negative-control check.

The host tests validate the control flow; they do not establish hardware timing,
firmware exclusivity or calibration. Actual hardware observations and their limits are documented below.

Run the host tests from the repository root with Python 3 and a host C compiler:

```bash
python3 -B -m unittest discover -s tests -p test_bc250_vrm_driver.py -v
```

These tests need no root privileges. They include 13 PAGE scenarios and 19
controller scenarios, plus a negative control that must reject a deliberately
broken restoration path. They do not load the module or access the board.

## Board validation (2026-11-04)

A BC-250 running BIOS P5.00 was tested with CachyOS kernel
`7.3.1-1-cachyos`. The existing customized fan driver received only the four
upstream EC-port-1 constants, preserving its locking and fan restoration.

- Both this driver and the fan driver built with `W=1` for the running Clang
  kernel and GCC-built `6.18.55-2-cachyos-lts`. DKMS installation succeeded for
  both kernels. Installed and loaded module source versions matched.
- Manual load, unload with verified CFG restoration, and reload succeeded.
  Initial CFG1/CFG2 were 0/3, equal to the applied values, and the first PMBus PAGE was 1; PAGE restoration
  and transport diagnostics stayed at zero. Fan automatic-mode restoration
  was separately exercised with the corrected fan driver.
- During a 47-second observation containing one completed inference request,
  22 samples showed CPU VRM 42–44 °C, GPU VRM 42–49 °C, and VIN 11.85–12.06 V.
  The connected fan stayed at 1197–1219 RPM while its existing controller ran.
  No kernel warnings were recorded during the manual-load trial.
- One subsequent reboot automatically loaded both installed modules, with
  matching source versions and zero VRM fault diagnostics. The existing fan,
  governor, model service and health checks recovered. AMDGPU display-IRQ
  warnings and the governor PCI warning also existed in the preceding boot;
  they are not claimed as warning-free startup.

Restoration from non-default CFG values was tested with host stubs only.
These are short functional observations, not sensor calibration or an endurance
test. Operation under the LTS kernel, suspend/resume, long-duration use, and
firmware/SMM concurrency have not been validated. P5 compatibility here applies
to VRM telemetry only; this driver does not measure GDDR6 temperature or patch
the SMU.
