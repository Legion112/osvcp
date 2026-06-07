# Hard Disk Drive Simulator

Simulator: [disk.py](../../../ostep-hw/file-disks/disk.py)

Upstream homework docs: [ostep-hw/file-disks/README.md](../../../ostep-hw/file-disks/README.md)

## Setup

```bash
cd exersises/persistence/hard-disk-drive
python3 -m venv --without-pip .venv
source .venv/bin/activate
```

No pip packages are required. If `python3 -m venv` fails, either install the venv module (`sudo apt install python3.12-venv`) or use `--without-pip` as shown above.

## Run

CLI mode (no system packages required):

```bash
python ../../../ostep-hw/file-disks/disk.py -a 10 -c
```

Graphical mode (requires system package):

```bash
sudo apt install python3-tk
python ../../../ostep-hw/file-disks/disk.py -a 10 -G
```

Press `s` to start the simulation, `q` to quit.

# Intro
Start: 6 
Target: 10
Speed 1
Per Track: 12
One sector: 30 time unit
105 time total
10 - 6 = 4 
3 * 30 = 90 
90 + 15 = 105  
105 + 30 = 135 total
135 + 30 = 165
```txt
TOTALS      Seek:  0  Rotate:105  Transfer: 30  Total: 135
```
## With seek

Notes:
Full rotation: 360 


## Questions
### 1.
Compute the seek, rotation, and transfer times for the following sets of requests: 
- `-a 0`
  - `360/2-15=165 Rotatin till start of sector 0
  - 30 reading sector 165 + 30 = 195
- `-a 6`
  - 360-15=345 rotation till start of sector 6
  - 30 reading sector 345 + 30 = 375 Total time
- `-a 30`
  - seek: 80
  - 360 - 80 = 280 - 15 = 265 till get to start of sector 30
  - 30 reading sector 265 + 30 = 295
- `-a 7,30,8` 
- `-a 10, 11, 12, 13`