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
  - `7`
    - 15 till start of sector 7
    - 30 read sector 7 = 15 + 30 = 45
  - `30`
    - seek: 80 
    - 10 sector till start of sector 30
    - 10 * 30 = 300 rotation from starting poit
    - 300 - 80 = 220 time spend for rotation 
    - 30 reading sector 220 + 30 = 250
    - sector total 80 + 250 = 330
    - total: 330 + 45 = 330 
  - `8`
    - seek: 80
    - Rotate:310
    - Transfer: 30
    - Total: 420
  - Result: Seek:160  Rotate:545  Transfer: 90  Total: 795
- `-a 10,11,12,13`
- Block:  10  Seek:  0  Rotate:105  Transfer: 30  Total: 135
  Block:  11  Seek:  0  Rotate:  0  Transfer: 30  Total:  30
  Block:  12  Seek: 40  Rotate:320  Transfer: 30  Total: 390
  Block:  13  Seek:  0  Rotate:  0  Transfer: 30  Total:  30
  TOTALS      Seek: 40  Rotate:425  Transfer:120  Total: 585
### 2 Do the same requests above, but change the seek rate to different values: -S 2, -S 4, -S 8, -S 10, -S 40, -S 0.1. How do the times change?
How do the times change when varying -S?

No seek → no effect. -a 0 and -a 6 are unchanged for all -S.

Seek and rotation trade off. For requests that need a seek, faster -S lowers seek time but the platter keeps spinning, so rotation often increases by the same amount. Total often stays the same (e.g. -a 30: 375 for S=1…40; -a 10,11,12,13: 585 for S=1…40).

Transfer time is unchanged (30 per sector).

Multi-track sequences can improve with faster seek. For -a 7,30,8, at S≥4 the return seek to sector 8 aligns much better → block 8 total drops from 420 to 60 and overall total from 795 to 435.

Very slow seek (S=0.1) hurts badly. Seek time dominates; totals jump (e.g. -a 30: 1095, -a 7,30,8: 2235, -a 10,11,12,13: 945).

Rule: seek speed affects how seek and rotation split the wait, not always the sum — except when slow seek changes rotational alignment or when faster seek lands the head near the next sector.
### 3 Do the same requests above, but change the rotation rate: -R 0.1, -R 0.5, -R 0.01. How do the times change?