#!/usr/bin/env python3
# ─── OpenRVBench :: Board Detection Script ───────────────────────────────────
# Standalone script to probe a RISC-V board and print a formatted info sheet.
# Can also be used as a module by the CLI.
# ─────────────────────────────────────────────────────────────────────────────
"""Detect RISC-V board capabilities and print a system report."""

import os
import pathlib
import platform
import re
import subprocess
import sys

# ─── sysfs helpers ───────────────────────────────────────────────────────────
def _read(path: str) -> str:
    try:
        return pathlib.Path(path).read_text().strip()
    except Exception:
        return ""


def detect_rvv_from_hwcap() -> bool:
    """Try /proc/cpuinfo ISA string for 'v' extension."""
    try:
        cpu = pathlib.Path("/proc/cpuinfo").read_text()
        for line in cpu.splitlines():
            if line.startswith("isa"):
                isa = line.split(":", 1)[1].strip()
                # RVV extensions: _v, _zve32x, _zvl, etc.
                return bool(re.search(r'_v_|_v$|\bv\b|[Vv]ext|zvl|zve', isa))
    except Exception:
        pass
    return False


def get_cpu_freqs() -> dict:
    """Read min/max/current frequencies from cpufreq."""
    freqs = {}
    cpu0 = "/sys/devices/system/cpu/cpu0/cpufreq"
    for key in ("cpuinfo_max_freq", "cpuinfo_min_freq", "scaling_cur_freq"):
        v = _read(f"{cpu0}/{key}")
        if v:
            try:
                freqs[key] = int(v) // 1000  # kHz → MHz
            except ValueError:
                pass
    return freqs


def get_thermal_zones() -> list:
    """List all thermal zones with type and temperature."""
    zones = []
    thermal_base = pathlib.Path("/sys/class/thermal")
    if not thermal_base.exists():
        return zones
    for zone_dir in sorted(thermal_base.glob("thermal_zone*")):
        z_type = _read(str(zone_dir / "type"))
        z_temp = _read(str(zone_dir / "temp"))
        try:
            temp_c = float(z_temp) / 1000.0
        except (ValueError, TypeError):
            temp_c = None
        zones.append({
            "zone": zone_dir.name,
            "type": z_type,
            "temp_c": temp_c,
        })
    return zones


def get_memory_info() -> dict:
    """Parse /proc/meminfo."""
    info = {}
    try:
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            parts = line.split()
            if len(parts) >= 2:
                key = parts[0].rstrip(":")
                try:
                    info[key] = int(parts[1])
                except ValueError:
                    info[key] = parts[1]
    except Exception:
        pass
    return info


def get_block_devices() -> list:
    """List block devices from /sys/block."""
    devices = []
    block = pathlib.Path("/sys/block")
    if not block.exists():
        return devices
    for dev in sorted(block.iterdir()):
        if dev.name.startswith("loop"):
            continue
        size_str = _read(str(dev / "size"))
        rotational = _read(str(dev / "queue/rotational"))
        try:
            size_gb = (int(size_str) * 512) / (1024**3)
        except (ValueError, TypeError):
            size_gb = 0.0
        dev_type = "HDD" if rotational == "1" else "SSD/eMMC/NVMe"
        devices.append({
            "name":     dev.name,
            "size_gb":  round(size_gb, 1),
            "type":     dev_type,
        })
    return devices


def get_network_interfaces() -> list:
    """List network interfaces."""
    ifaces = []
    net = pathlib.Path("/sys/class/net")
    if not net.exists():
        return ifaces
    for iface in sorted(net.iterdir()):
        speed_str = _read(str(iface / "speed"))
        carrier   = _read(str(iface / "carrier"))
        op_state  = _read(str(iface / "operstate"))
        try:
            speed = int(speed_str)
        except (ValueError, TypeError):
            speed = -1
        ifaces.append({
            "name":      iface.name,
            "speed_mbs": speed,
            "up":        (carrier == "1"),
            "state":     op_state,
        })
    return ifaces


# ─── Main probe ───────────────────────────────────────────────────────────────
def probe_board() -> dict:
    """Collect complete board information."""
    info = {}

    # Board name
    for path in ["/proc/device-tree/model",
                 "/sys/firmware/devicetree/base/model"]:
        v = _read(path)
        if v:
            info["board_name"] = v.rstrip("\x00")
            break
    else:
        info["board_name"] = "Unknown RISC-V Board"

    # ISA
    isa = ""
    try:
        for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("isa"):
                isa = line.split(":", 1)[1].strip()
                break
    except Exception:
        pass
    info["isa"] = isa or "unknown"
    info["has_rvv"] = detect_rvv_from_hwcap()
    info["cores"]   = os.cpu_count() or 1

    # Frequencies
    info["cpu_freq"] = get_cpu_freqs()

    # Memory
    meminfo = get_memory_info()
    info["ram_total_mb"] = meminfo.get("MemTotal", 0) // 1024
    info["ram_free_mb"]  = meminfo.get("MemAvailable", 0) // 1024

    # OS / Kernel
    info["kernel"] = platform.release()
    try:
        for line in pathlib.Path("/etc/os-release").read_text().splitlines():
            if line.startswith("PRETTY_NAME="):
                info["os"] = line.split("=", 1)[1].strip('"')
                break
        else:
            info["os"] = platform.system()
    except Exception:
        info["os"] = platform.system()

    # Thermal
    info["thermal_zones"] = get_thermal_zones()

    # Storage
    info["block_devices"] = get_block_devices()

    # Network
    info["network_ifaces"] = get_network_interfaces()

    return info


# ─── Formatter ────────────────────────────────────────────────────────────────
def print_board_report(info: dict):
    W = 65
    def _sep(title=""):
        if title:
            pad = "─" * max(1, W - len(title) - 4)
            print(f"  ── {title} {pad}")
        else:
            print("  " + "─" * W)

    print()
    print("  ╔" + "═" * W + "╗")
    print(f"  ║  OpenRVBench :: Board Probe Report" + " " * (W - 35) + "║")
    print("  ╚" + "═" * W + "╝")
    print()

    _sep("Board")
    print(f"  {'Board':<18} {info.get('board_name', '?')}")
    print(f"  {'ISA':<18} {info.get('isa', '?')}")
    rvv = "✓ RVV available" if info.get("has_rvv") else "✗ No RVV"
    print(f"  {'Vector Ext':<18} {rvv}")
    print(f"  {'CPU Cores':<18} {info.get('cores', '?')}")

    freq = info.get("cpu_freq", {})
    if freq:
        fmin = freq.get("cpuinfo_min_freq", "?")
        fmax = freq.get("cpuinfo_max_freq", "?")
        fcur = freq.get("scaling_cur_freq", "?")
        print(f"  {'CPU Freq':<18} {fmin}–{fmax} MHz  (cur: {fcur} MHz)")

    print()
    _sep("Memory")
    ram_total = info.get("ram_total_mb", 0)
    ram_free  = info.get("ram_free_mb",  0)
    print(f"  {'Total RAM':<18} {ram_total:,} MB  ({ram_total/1024:.1f} GB)")
    print(f"  {'Available RAM':<18} {ram_free:,} MB")

    print()
    _sep("OS")
    print(f"  {'OS':<18} {info.get('os', '?')}")
    print(f"  {'Kernel':<18} {info.get('kernel', '?')}")

    print()
    _sep("Thermal Zones")
    zones = info.get("thermal_zones", [])
    if zones:
        for z in zones:
            temp = f"{z['temp_c']:.1f}°C" if z["temp_c"] is not None else "n/a"
            print(f"  {z['zone']:<18} {z['type']:<20} {temp}")
    else:
        print("  No thermal zones found")

    print()
    _sep("Storage")
    devs = info.get("block_devices", [])
    if devs:
        for d in devs:
            print(f"  {d['name']:<18} {d['size_gb']:>8.1f} GB  {d['type']}")
    else:
        print("  No block devices found")

    print()
    _sep("Network")
    ifaces = info.get("network_ifaces", [])
    for iface in ifaces:
        speed = f"{iface['speed_mbs']} Mb/s" if iface["speed_mbs"] > 0 else "unknown"
        state = "UP" if iface["up"] else "down"
        print(f"  {iface['name']:<18} {state:<6} {speed}")

    print()
    _sep()
    print()


if __name__ == "__main__":
    import json as _json
    info = probe_board()
    if "--json" in sys.argv:
        print(_json.dumps(info, indent=2))
    else:
        print_board_report(info)
