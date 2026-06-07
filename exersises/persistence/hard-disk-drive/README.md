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

## Reference (default disk parameters)

| Parameter | Default value |
|-----------|---------------|
| Starting position | Outer track, halfway through sector 6 |
| Rotation speed (`-R`) | 1 degree / time unit (full revolution = 360) |
| Seek speed (`-S`) | 1 distance unit / time unit |
| Track spacing | 40 distance units per track |
| Sectors per track | 12 (30° each → 30 time units rotation per sector) |
| Transfer | 30 time units per sector at `-R 1` |
| Per-request total | Seek + Rotate + Transfer |

Track layout (from simulator block map):

- Track 0 (outer): sectors 0–11
- Track 1 (middle): sectors 12–23
- Track 2 (inner): sectors 24–35

---

## Report

### Question 1 — Compute seek, rotation, and transfer times

Command used: `python ../../../ostep-hw/file-disks/disk.py -a <addrs> -c`

#### `-a 0`

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 0 | 0 | 165 | 30 | **195** |

**Calculation:** Head starts halfway through sector 6. Sector 0 start is half a revolution away (180°), minus 15° to the read point between sectors −1/0 → **165** rotation. Transfer **30**. No seek (same track).

#### `-a 6`

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 6 | 0 | 345 | 30 | **375** |

**Calculation:** From halfway through sector 6, one full revolution (360°) minus 15° to the read point between 5 and 6 → **345** rotation. Transfer **30**. No seek.

#### `-a 30`

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 30 | 80 | 265 | 30 | **375** |

**Calculation:** Sector 30 is on the inner track (track 2). Seek = 2 tracks × 40 = **80**. Zero-cost rotation from start to sector 30 start = 280°; disk spins 80° during seek → **280 − 80 = 265** rotation. Transfer **30**.

#### `-a 7,30,8`

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 7 | 0 | 15 | 30 | **45** |
| 30 | 80 | 220 | 30 | **330** |
| 8 | 80 | 310 | 30 | **420** |
| **TOTALS** | **160** | **545** | **90** | **795** |

**Block 7:** 15° rotation to read point between 6 and 7, plus 30 transfer.

**Block 30:** After reading 7, seek 2 tracks inward = **80**. From end-of-read position (aligned with ~sector 31 on inner track), 10 sectors CCW to sector 30 start = 300°; minus 80° during seek → **220** rotation. Total per block = 80 + 220 + 30 = **330** (not cumulative with block 7).

**Block 8:** Seek 2 tracks back to outer = **80**. Rotation **310**, transfer **30**.

#### `-a 10,11,12,13`

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 10 | 0 | 105 | 30 | **135** |
| 11 | 0 | 0 | 30 | **30** |
| 12 | 40 | 320 | 30 | **390** |
| 13 | 0 | 0 | 30 | **30** |
| **TOTALS** | **40** | **425** | **120** | **585** |

**Block 10:** 105° rotation (same as intro example). **Block 11:** next sector, no extra rotation. **Block 12:** seek 1 track to middle (**40**); rotation accounts for platter spin during seek (**320**). **Block 13:** adjacent sector on same track, no rotation wait.

---

### Question 2 — Vary seek rate (`-S 2, -S 4, -S 8, -S 10, -S 40, -S 0.1`)

`-S` is seek speed (distance units per time unit). Seek time = track distance / S. The platter keeps spinning during a seek, so **seek and rotation trade off**.

#### Summary (TOTALS)

| Request set | S=1 | S=2 | S=4 | S=8 | S=10 | S=40 | S=0.1 |
|-------------|-----|-----|-----|-----|------|------|-------|
| `-a 0` | 195 | 195 | 195 | 195 | 195 | 195 | 195 |
| `-a 6` | 375 | 375 | 375 | 375 | 375 | 375 | 375 |
| `-a 30` | 375 | 375 | 375 | 375 | 375 | 375 | **1095** |
| `-a 7,30,8` | 795 | 795 | **435** | **435** | **435** | **435** | **2235** |
| `-a 10,11,12,13` | 585 | 585 | 585 | 585 | 585 | 585 | **945** |

#### Findings

1. **No seek → no effect.** `-a 0` and `-a 6` are unchanged for all `-S`.

2. **Seek and rotation trade off.** Faster `-S` lowers seek time but increases rotation wait by the same amount. **Total often stays the same** (e.g. `-a 30` stays 375 for S=1…40; `-a 10,11,12,13` stays 585).

3. **Transfer time is unchanged** (30 per sector at default `-R`).

4. **Multi-track sequences can improve with faster seek.** For `-a 7,30,8` at S≥4, the fast return seek to sector 8 lands the head much closer to the target — block 8 rotation drops from 310 to ~10–28, block total from 420 to **60**, overall total from **795 to 435**.

5. **Very slow seek (S=0.1) hurts badly.** Seek time dominates; totals jump sharply (`-a 30`: 1095, `-a 7,30,8`: 2235, `-a 10,11,12,13`: 945).

**Rule:** Seek speed changes how seek and rotation split the wait, not always the sum — except when slow seek changes rotational alignment, or when faster seek lands the head near the next sector.

Example (`-a 30`, S=4):

```
Block:  30  Seek: 20  Rotate:325  Transfer: 30  Total: 375
```

---

### Question 3 — Vary rotation rate (`-R 0.1, -R 0.5, -R 0.01`)

`-R` is rotation speed in degrees per time unit. Slower rotation increases both **rotation wait** and **transfer time** (~ scales as 1/R). **Seek time is unaffected.**

#### Summary (TOTALS)

| Request set | R=1 | R=0.5 | R=0.1 | R=0.01 |
|-------------|-----|-------|-------|--------|
| `-a 0` | 195 | 390 (×2) | 1950 (×10) | 19500 (×100) |
| `-a 6` | 375 | 750 (×2) | 3750 (×10) | 37501 (×100) |
| `-a 30` | 375 | 750 (×2) | 3750 (×10) | 37501 (×100) |
| `-a 7,30,8` | 795 | 1590 (×2) | **4349** | **43500** |
| `-a 10,11,12,13` | 585 | 1170 (×2) | 5850 (×10) | 58501 (×100) |

#### Findings

1. **Seek time never changes.** Example: `-a 30` seek stays **80** at every `-R`.

2. **Rotation and transfer both grow** as `-R` decreases (30 → 60 → 300 → 3000 transfer per sector).

3. **No-seek requests scale uniformly:** `-a 0` and `-a 6` totals multiply by ~1/R.

4. **With seek:** total ≈ seek + (rotate + transfer) × (1/R). Example `-a 30`: 80 + 295/R → 750 at R=0.5, 3750 at R=0.1.

5. **Multi-track sequences may not scale evenly** at extreme `-R` because slower spin during a seek changes sector alignment. Example `-a 7,30,8` at R=0.1: block 8 rotation **drops** from 310 to 219 (less spin during the 80-unit seek), so total is 4349 rather than a clean ×10.

6. **Compared to Q2 (`-S`):** seek rate trades seek vs rotation while total often stays fixed; rotation rate increases both rotation and transfer while seek stays fixed — totals **always grow** as `-R` drops.

Example (`-a 0`, R=0.5):

```
Block:   0  Seek:  0  Rotate:330  Transfer: 60  Total: 390
```

Example (`-a 7,30,8`, R=0.1 — note block 8 rotation anomaly):

```
Block:   7  Seek:  0  Rotate:150  Transfer:299  Total: 449
Block:  30  Seek: 80  Rotate:2920  Transfer:301  Total:3301
Block:   8  Seek: 80  Rotate:219  Transfer:300  Total: 599
TOTALS      Seek:160  Rotate:3289  Transfer:900  Total:4349
```

---

### Comparison: `-S` vs `-R`

| | Seek time | Rotation | Transfer | Typical total effect |
|---|-----------|----------|----------|----------------------|
| **Higher `-S` (faster seek)** | Decreases | Often increases | Unchanged | Often unchanged; can improve multi-track alignment |
| **Lower `-R` (slower spin)** | Unchanged | Increases | Increases | Always increases (~1/R for rotate+transfer) |
