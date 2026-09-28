# AMD BC-250 VRM hwmon driver

Linux kernel `hwmon` driver for reading voltage, current, temperature, and power metrics from the onboard power management IC (PMIC / VRM) on the **AMD BC-250 (Cyan Skillfish)** board.

It reads PMBus Page 0 (CPU VRM) and Page 1 (GPU VRM) over SMBus/I2C and exposes standard, labeled `/sys/class/hwmon/` sensors readable by `lm_sensors` (`sensors`), `CoolerControl`, `MangoHud`, etc.

> **Hardware Setup Required**: Exposing SMBus I2C telemetry lines on the BC-250 requires a small hardware modification. Refer to the [BC250 Telemetry Hardware Guide](https://github.com/onlinermm/BC250-Telemetry/blob/main/hardware.md) for details.

## Features

Exposes the following sensors and hardware limits in `lm_sensors`:
- **`VIN (12V Input)`**: Main input voltage (mV)
- **`CPU Voltage`**: CPU rail voltage (mV) + Over-Voltage Limit (`in1_max`)
- **`GPU Core Voltage`**: GPU rail voltage (mV) + Over-Voltage Limit (`in2_max`)
- **`CPU Current`**: CPU rail current (mA) + Over-Current Critical Limit (`curr1_crit`)
- **`GPU Current`**: GPU rail current (mA) + Over-Current Critical Limit (`curr2_crit`)
- **`CPU VRM Temp`**: CPU VRM temperature (°C) + Warning/Critical Limits (`temp1_max`, `temp1_crit`)
- **`GPU VRM Temp`**: GPU VRM temperature (°C) + Warning/Critical Limits (`temp2_max`, `temp2_crit`)
- **`CPU Power`**: Calculated CPU power draw (W)
- **`GPU Power`**: Calculated GPU power draw (W)

---

## Automatic Detection & Loading

The driver includes built-in **DMI board matching** (`AMD BC-250`) and **I2C auto-probing** (`0x60`). When installed, `udev` automatically loads the module on boot and binds to the VRM hardware with zero manual configuration or systemd services needed.

---

## DKMS Installation (Auto-Rebuild on Kernel Updates)

### 1. Copy source to `/usr/src/`
```bash
sudo cp -r . /usr/src/bc250-vrm-1.0.0
```

### 2. Register & build with DKMS
```bash
sudo dkms add -m bc250-vrm -v 1.0.0
sudo dkms build -m bc250-vrm -v 1.0.0
sudo dkms install -m bc250-vrm -v 1.0.0
```
