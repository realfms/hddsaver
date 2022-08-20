# hddsaver
ASRock HDD Saver Linux kernel patches

Some of the asrock motherboards are equipped with hdd saver technology.

It is implemented via a special power connector on the motherboard and a corresponding custom made power cable with two SATA power ports.

The control is via the gpio10 pin of the nct6791d chip.

When this technology is available the module creates the hddsaver_enable file.

Reading the file returns the current status, while writing turns off or on the power of the SATA connectors.

Supported boards:
- Tested
	- Z97 Extreme4
- Not tested
	- Fatal1ty X99X Killer
	- Fatal1ty X99X Killer/3.1
	- Fatal1ty Z97 Professional
	- Fatal1ty Z97X Killer
	- X99 Extreme11
	- X99 Extreme4
	- X99 Extreme4/3.1
	- X99 Extreme3
	- X99 Extreme6
	- X99 Extreme6/ac
	- X99 Extreme6/3.1
	- X99 OC Formula
	- X99 OC Formula/3.1
	- X99 WS
	- X99M Extreme4
	- Z97 Extreme4/3.1
	- Z97 Extreme6
	- Z97 Extreme6/3.1
	- Z97 Extreme6/ac
	- Z97 Extreme9
	- Z97 OC Formula
