# hddsaver
ASRock HDD Saver Linux kernel patches

Some of the asrock motherboards are equipped with hdd saver technology.

It is implemented via a special power connector on the motherboard and a corresponding custom made power cable with two SATA power ports.

The control is via the gpio10 pin of the nct6791d chip.

When this technology is available the module creates the hddsaver_enable file.

Reading the file returns the current status, while writing turns off or on the power of the SATA connectors.

Supported boards:
- Z97 Extreme4 - tested
- Z97 Extreme6 - not tested
- X99 Extreme4/3.1 - not tested
