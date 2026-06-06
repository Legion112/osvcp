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