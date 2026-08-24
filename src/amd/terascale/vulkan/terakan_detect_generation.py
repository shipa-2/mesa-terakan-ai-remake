#!/usr/bin/env python3
# Copyright © 2026 Terakan contributors
# SPDX-License-Identifier: MIT

"""Picks a default value for the terakan-target-generation Meson option.

Scans the PCI devices of the build machine for an AMD/ATI TeraScale GPU, matches
its device ID against the chip family table Terakan already uses at runtime, and
prints which of the four TeraScale generations (r600, r700, r800, r900) it
belongs to. This only labels the build; the driver itself detects the chip
family of the GPU it actually runs on at Vulkan instance/device creation time,
the same as it always has. Nothing here changes runtime behavior.

Prints one line: "r600", "r700", "r800", "r900", or "none" if no AMD PCI device
was found, or "ambiguous" if AMD devices from more than one generation were
found. The caller falls back to r900 (Terakan's primary supported target,
Northern Islands) in either of those last two cases.
"""

import re
import sys

# Same generation split CHIP_* families fall into in AMD's own family enum
# (src/amd/common/amd_family.h) and in include/pci_ids/r600_pci_ids.h: R600 is
# TeraScale 1 (HD2000/3000), R700 is its refresh (HD4000), R800 is Evergreen
# (TeraScale 2, HD5000, plus its Palm/Sumo/Sumo2 APU variants), and R900 is
# Northern Islands (TeraScale 2 refresh plus the VLIW4 Cayman/Aruba pair,
# HD6000). This is the marketing-generation split, not the VLIW4-vs-VLIW5 ISA
# split the driver uses internally at runtime (there Barts/Turks/Caicos are
# grouped with Evergreen, since they share its instruction encoding).
GENERATION_FAMILIES = {
   "r600": {"R600", "RV610", "RV620", "RV630", "RV635", "RV670", "RS780", "RS880"},
   "r700": {"RV710", "RV730", "RV740", "RV770"},
   "r800": {"CEDAR", "REDWOOD", "JUNIPER", "CYPRESS", "HEMLOCK", "PALM", "SUMO", "SUMO2"},
   "r900": {"BARTS", "TURKS", "CAICOS", "CAYMAN", "ARUBA"},
}


def load_pci_id_families(pci_ids_path):
   """Returns {device_id: family_name} from a CHIPSET(id, name, family) header."""
   pattern = re.compile(r"CHIPSET\(\s*(0x[0-9A-Fa-f]+)\s*,\s*\w+\s*,\s*(\w+)\s*\)")
   families = {}
   with open(pci_ids_path, encoding="utf-8") as pci_ids_file:
      for line in pci_ids_file:
         match = pattern.match(line.strip())
         if match:
            families[int(match.group(1), 16)] = match.group(2)
   return families


def read_hex_sysfs_file(path):
   try:
      with open(path, encoding="utf-8") as sysfs_file:
         return int(sysfs_file.read().strip(), 16)
   except (OSError, ValueError):
      return None


def find_local_amd_device_ids():
   import glob

   device_ids = []
   for vendor_path in glob.glob("/sys/bus/pci/devices/*/vendor"):
      if read_hex_sysfs_file(vendor_path) != 0x1002:
         continue
      device_id = read_hex_sysfs_file(vendor_path[: -len("vendor")] + "device")
      if device_id is not None:
         device_ids.append(device_id)
   return device_ids


def generation_of_family(family_name):
   for generation, families in GENERATION_FAMILIES.items():
      if family_name in families:
         return generation
   return None


def main():
   if len(sys.argv) != 2:
      print("usage: terakan_detect_generation.py PCI_IDS_HEADER", file=sys.stderr)
      return 2

   id_to_family = load_pci_id_families(sys.argv[1])
   generations_found = set()
   for device_id in find_local_amd_device_ids():
      family_name = id_to_family.get(device_id)
      if family_name is None:
         continue
      generation = generation_of_family(family_name)
      if generation is not None:
         generations_found.add(generation)

   if len(generations_found) == 0:
      print("none")
   elif len(generations_found) == 1:
      print(next(iter(generations_found)))
   else:
      print("ambiguous")
   return 0


if __name__ == "__main__":
   sys.exit(main())
