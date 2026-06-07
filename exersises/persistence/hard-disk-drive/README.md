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

---

### Question 4 — FIFO vs SSTF for `-a 7,30,8`

FIFO is not always best. For this workload, SSTF should service **7 → 8 → 30** instead of FIFO's **7 → 30 → 8**.

**Why:** After reading sector 7, the head is on the outer track near sector 8. SSTF picks the request with the shortest seek distance next:

- Sector **8** — same track, seek **0**
- Sector **30** — inner track, seek **80**

So SSTF serves 8 before making the long seek to 30.

Commands:

```bash
python ../../../ostep-hw/file-disks/disk.py -a 7,30,8 -c          # FIFO (default)
python ../../../ostep-hw/file-disks/disk.py -a 7,30,8 -p SSTF -c  # SSTF
```

#### FIFO (default policy)

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 7 | 0 | 15 | 30 | **45** |
| 30 | 80 | 220 | 30 | **330** |
| 8 | 80 | 310 | 30 | **420** |
| **TOTALS** | **160** | **545** | **90** | **795** |

Order: **7 → 30 → 8**. After sector 7, FIFO seeks all the way to the inner track for 30, then seeks back to outer for 8 — two expensive 80-unit seeks.

#### SSTF (`-p SSTF`)

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 7 | 0 | 15 | 30 | **45** |
| 8 | 0 | 0 | 30 | **30** |
| 30 | 80 | 190 | 30 | **300** |
| **TOTALS** | **80** | **205** | **90** | **375** |

Order: **7 → 8 → 30**.

**Block 7:** Same as FIFO — 15° rotation, 30 transfer.

**Block 8:** No seek (adjacent sector on same track after reading 7). No rotation wait — read point already under head. Transfer only → **30**.

**Block 30:** One seek inward (**80**). From end-of-read on outer track (between 7 and 8), CCW to sector 30 start = 270° zero-cost rotation; minus 80° during seek → **190** rotation. Transfer **30**. Per-block total **300**.

#### Comparison

| Policy | Order | Total seek | Total rotate | Total transfer | **Total time** |
|--------|-------|------------|--------------|----------------|----------------|
| FIFO | 7 → 30 → 8 | 160 | 545 | 90 | **795** |
| SSTF | 7 → 8 → 30 | 80 | 205 | 90 | **375** |

SSTF cuts total time roughly in half (**795 → 375**) by eliminating one 80-unit seek and reading sector 8 while the head is already on the outer track.

**Answer:** Process requests in order **7, 8, 30**. With `-p SSTF`, each request takes:

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 7 | 0 | 15 | 30 | 45 |
| 8 | 0 | 0 | 30 | 30 |
| 30 | 80 | 190 | 30 | 300 |

Simulator output:

```
Block:   7  Seek:  0  Rotate: 15  Transfer: 30  Total:  45
Block:   8  Seek:  0  Rotate:  0  Transfer: 30  Total:  30
Block:  30  Seek: 80  Rotate:190  Transfer: 30  Total: 300
TOTALS      Seek: 80  Rotate:205  Transfer: 90  Total: 375
```

---

### Question 5 — SATF vs SSTF (`-p SATF`)

**SATF** (Shortest Access Time First) picks the request with the smallest estimated **seek + rotation + transfer**. **SSTF** (Shortest Seek Time First) only minimizes seek distance; when several blocks share the nearest track, SSTF then uses SATF among them.

Commands:

```bash
python ../../../ostep-hw/file-disks/disk.py -a 7,30,8 -p SATF -c
python ../../../ostep-hw/file-disks/disk.py -a 6,20 -p SATF -c
python ../../../ostep-hw/file-disks/disk.py -a 6,20,8 -p SSTF -c
python ../../../ostep-hw/file-disks/disk.py -a 6,20,8 -p SATF -c
```

#### Does SATF differ for `-a 7,30,8`?

**No.** SATF and SSTF produce identical results:

| Policy | Order | Seek | Rotate | Transfer | **Total** |
|--------|-------|------|--------|----------|-----------|
| SSTF | 7 → 8 → 30 | 80 | 205 | 90 | **375** |
| SATF | 7 → 8 → 30 | 80 | 205 | 90 | **375** |

After serving sector 7, sector 8 wins on **both** metrics:

- **Shortest seek:** 8 is on the same track (0) vs 30 (80)
- **Shortest access time:** 8 needs 0 rotation; 30 needs 80 seek + 190 rotation

There is no conflict between seek and rotation here, so SATF adds nothing beyond SSTF.

#### Example where SATF outperforms SSTF: `-a 6,20`

| Policy | Order | Seek | Rotate | Transfer | **Total** |
|--------|-------|------|--------|----------|-----------|
| FIFO | 6 → 20 | 40 | 695 | 60 | **795** |
| SSTF | 6 → 20 | 40 | 695 | 60 | **795** |
| **SATF** | **20 → 6** | 80 | 235 | 60 | **375** |

**SSTF** serves sector 6 first (same track, seek 0) — but that requires **345** rotation from the default start position. **SATF** notices sector 20 on the middle track is rotationally aligned: one 40-unit seek plus only **5** rotation → total **75** for block 20, then seek back for block 6.

SATF per-request breakdown:

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 20 | 40 | 5 | 30 | **75** |
| 6 | 40 | 230 | 30 | **300** |
| **TOTALS** | **80** | **235** | **60** | **375** |

#### Stronger 3-request example: `-a 6,20,8`

| Policy | Order | **Total** |
|--------|-------|-----------|
| FIFO | 6 → 20 → 8 | 1155 |
| SSTF | 8 → 6 → 20 | 795 |
| **SATF** | **20 → 6 → 8** | **435** |

SATF beats SSTF by **360** time units (795 → 435).

SATF per-request breakdown:

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 20 | 40 | 5 | 30 | **75** |
| 6 | 40 | 230 | 30 | **300** |
| 8 | 0 | 30 | 30 | **60** |
| **TOTALS** | **80** | **265** | **90** | **435** |

SSTF serves outer-track requests first (8 → 6 → 20) because they have shorter seeks, but still pays **350** rotation for block 20. SATF serves the rotationally favorable middle-track block 20 first, then recovers outer-track requests cheaply.

#### When is SATF better than SSTF?

| Situation | Why SATF wins |
|-----------|---------------|
| A farther track is **rotationally close** (sector about to pass the head after a short seek) | SATF picks it; SSTF waits for a nearer track that requires nearly a full revolution |
| **Seek vs rotation trade-off** — same-track request needs long rotation, cross-track request needs short seek + little rotation | SATF minimizes total access time; SSTF blindly prefers zero seek |
| Initial head position favors a non-nearest track | Example: `-a 6,20` from default start (halfway through sector 6) |

#### When is SSTF better than SATF?

SATF can be **greedy** and hurt overall performance:

| Example | SSTF total | SATF total | What goes wrong |
|---------|------------|------------|-----------------|
| `-a 10,20` | **435** | 495 | SATF serves 20 first (75), then 10 costs 420 — nearly a full rotation back |

SSTF keeps 10 → 20 (same middle track after first read), avoiding the expensive return seek + rotation.

#### Summary

| Question | Answer |
|----------|--------|
| SATF vs SSTF for `-a 7,30,8`? | **No difference** — both yield 375 (order 7 → 8 → 30) |
| Request set where SATF beats SSTF? | **`-a 6,20`** (795 → 375) or **`-a 6,20,8`** (795 → 435) |
| When is SATF better? | When a request on a **farther track** has **lower total access time** because it is rotationally aligned, and SSTF's seek-only view would pick a nearer track that requires waiting almost a full spin |
| When is SSTF better? | When serving a rotationally favorable far request first **breaks sequential locality** on the nearer track, making later requests much more expensive |

**Rule of thumb:** SSTF optimizes **distance**; SATF optimizes **time**. They differ when seek distance and rotational position disagree.

---

### Question 6 — Track skew for `-a 10,11,12,13`

#### What is track skew?

Real disks store more sectors on outer tracks than inner tracks. When the OS reads sequential blocks (10, 11, 12, 13), block 12 is often on the **next inward track**. While the head **seeks** inward, the platter keeps spinning.

Without skew, sector 12 on the middle track sits at the **same angular position** as sector 0 on the outer track. After reading sector 11, the head seeks one track inward — but sector 12 has already spun past. The drive waits almost a **full revolution** (~320 time units) before it can read block 12.

**Track skew** offsets sectors on inner tracks forward (in the direction of rotation) so that, after a one-track seek, the next sequential sector is **under the head**. In this simulator, `-o N` shifts the middle track by **N blocks** and the inner track by **2N blocks**:

```text
middle track angle += N × 30°
inner track angle  += 2N × 30°
```

(30° = one sector at default zoning.)

#### What goes poorly without skew (`-o 0`)?

```bash
python ../../../ostep-hw/file-disks/disk.py -a 10,11,12,13 -c
```

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 10 | 0 | 105 | 30 | 135 |
| 11 | 0 | 0 | 30 | **30** |
| 12 | 40 | **320** | 30 | **390** |
| 13 | 0 | 0 | 30 | **30** |
| **TOTALS** | **40** | **425** | **120** | **585** |

- **Blocks 10 → 11:** same outer track, sequential — rotate **0**. Good.
- **Block 12:** one-track seek inward — seek is only 40, but rotation is **320** (nearly a full spin). This is the bottleneck.
- **Block 13:** same middle track as 12, adjacent — rotate **0**. Good.

**Problem:** the outer → middle transition (11 → 12) pays a huge rotational penalty because sector 12 is not skewed to account for seek time.

#### Skew experiments (default `-S 1`, `-R 1`)

| Skew (`-o`) | Block 12 rotate | Block 12 total | **Overall total** |
|-------------|-----------------|----------------|-------------------|
| 0 | 320 | 390 | 585 |
| 1 | 350 | 420 | 615 |
| **2** | **20** | **90** | **285** ← best |
| 3 | 50 | 120 | 315 |
| 12 | 320 | 390 | 585 |

**Optimal skew at default seek rate: `-o 2`**

```bash
python ../../../ostep-hw/file-disks/disk.py -a 10,11,12,13 -o 2 -c
```

| Block | Seek | Rotate | Transfer | Total |
|-------|------|--------|----------|-------|
| 10 | 0 | 105 | 30 | 135 |
| 11 | 0 | 0 | 30 | 30 |
| 12 | 40 | **20** | 30 | **90** |
| 13 | 0 | 0 | 30 | 30 |
| **TOTALS** | **40** | **125** | **120** | **285** |

Block 12 rotation drops from **320 → 20**. Total time **585 → 285** (saved 300 time units).

#### Skew at different seek rates

Seek and rotation trade off during a track change — faster seek means less platter rotation during the seek, so **less skew** is needed.

| Seek rate (`-S`) | One-track seek time | Optimal skew | Total time |
|------------------|---------------------|--------------|------------|
| 1 (default) | 40 / 1 = **40** | **2** | **285** |
| 2 | 40 / 2 = **20** | **1** | **255** |
| 4 | 40 / 4 = **10** | **1** | **255** |

**`-S 2 -o 1`:**

```
Block:  12  Seek: 20  Rotate: 10  Transfer: 30  Total:  60
TOTALS      Seek: 20  Rotate:115  Transfer:120  Total: 255
```

**`-S 4 -o 1`:**

```
Block:  12  Seek: 10  Rotate: 20  Transfer: 30  Total:  60
TOTALS      Seek: 10  Rotate:125  Transfer:120  Total: 255
```

At S=2 and S=4, skew=1 minimizes total time. Seek time shrinks but block 12 still needs a small rotation (10–20) because one block of skew is slightly more than strictly needed — still far better than skew=0.

#### General skew formula

For a **one-track inward** step in this simulator:

```text
T_seek     = track_width / S          (track_width = 40 by default)
T_sector   = sector_angle / R         (sector_angle = 30° by default, R = rotate speed)

skew_blocks = ceil(T_seek / T_sector)
            = ceil(track_width / (S × T_sector))
            = ceil(40 / (30 × S))     at default R=1
```

With rotation rate `-R`:

```text
skew_blocks = ceil(track_width × R / (sector_angle × S))
            = ceil(40 × R / (30 × S))
```

**Examples (R=1):**

| S | T_seek | skew = ceil(40/(30×S)) |
|---|--------|-------------------------|
| 1 | 40 | ceil(1.33) = **2** |
| 2 | 20 | ceil(0.67) = **1** |
| 4 | 10 | ceil(0.33) = **1** |
| 8 | 5 | ceil(0.17) = **1** |

**Intuition:** during a one-track seek, the disk rotates through `T_seek / T_sector` sector widths. Skew the next track forward by that many sectors (rounded up) so the next sequential block arrives at the head right when the seek completes.

For the **inner track** (two tracks from outer), the simulator applies **2× skew** automatically (`-o N` → middle gets N blocks, inner gets 2N).

#### Summary

| Question | Answer |
|----------|--------|
| What goes poorly? | Block **12** after cross-track seek — **320** rotation (almost full revolution) while blocks 11 and 13 need none |
| Fix | Track skew (`-o`) offsets inner-track sectors forward |
| Best skew at `-S 1` | **`-o 2`** → total **285** |
| Best skew at `-S 2`, `-S 4` | **`-o 1`** → total **255** |
| Formula | **`skew = ceil(track_width / (S × T_sector))`** where `T_sector = sector_angle / R` |

---

### Question 7 — Zoning (`-z 10,20,30`) and per-track bandwidth

#### What is zoning?

By default (`-z 30,30,30`), every track has the same angular spacing between sectors (30°), so **12 sectors per track**.

**Zoning** models a real disk where **outer tracks hold more sectors** than inner tracks (same linear density, but outer circumference is longer). The `-z` flag sets the **angular width per sector** on each track:

```text
-z outer,middle,inner   (degrees between sector centers)
```

With **`-z 10,20,30`**:

| Track | Zone angle | Sectors per track | Block numbers |
|-------|------------|-------------------|---------------|
| Outer (0) | 10° | 360/10 = **36** | 0–35 |
| Middle (1) | 20° | 360/20 = **18** | 36–53 |
| Inner (2) | 30° | 360/30 = **12** | 54–65 |

Outer sectors are **narrower** (10°) → shorter transfer time per sector. Inner sectors are **wider** (30°) → longer transfer. Total disk capacity: 36 + 18 + 12 = **66 blocks**.

Command:

```bash
python ../../../ostep-hw/file-disks/disk.py -z 10,20,30 -a -1 -A 5,-1,0 -s <seed> -c
```

#### Random request experiments (different seeds)

Five random requests from 0 to max block (65), FIFO, default `-S 1 -R 1`:

| Seed | Requests | Seek | Rotate | Transfer | **Total** |
|------|----------|------|--------|----------|-----------|
| 0 | 45, 40, 22, 13, 27 | 80 | 1025 | 70 | **1175** |
| 1 | 7, 45, 41, 13, 26 | 80 | 1015 | 70 | **1165** |
| 2 | 51, 51, 3, 4, 45 | 120 | 530 | 80 | **730** |
| 3 | 12, 29, 19, 32, 33 | 0 | 825 | 50 | **875** |
| 7 | 17, 8, 35, 3, 28 | 0 | 1135 | 50 | **1185** |
| 42 | 34, 1, 14, 12, 39 | 40 | 870 | 60 | **970** |

**Seed 0 detail:**

```
Block:  45  Seek: 40  Rotate:310  Transfer: 20  Total: 370   (middle track)
Block:  40  Seek:  0  Rotate:240  Transfer: 20  Total: 260   (middle)
Block:  22  Seek: 40  Rotate: 85  Transfer: 10  Total: 135   (outer)
Block:  13  Seek:  0  Rotate:260  Transfer: 10  Total: 270   (outer)
Block:  27  Seek:  0  Rotate:130  Transfer: 10  Total: 140   (outer)
TOTALS      Seek: 80  Rotate:1025  Transfer: 70  Total:1175
```

**Observations from random workloads:**

- **Transfer times vary by zone:** 10 (outer), 20 (middle), 30 (inner) at default `-R 1` — matches the `-z` angles.
- **Rotation dominates** total time (825–1135 vs seek 0–120) because random requests scatter across tracks.
- **Same seed can repeat blocks** (seed 2: block 51 twice) — second access may need large rotation.
- Totals vary widely (730–1185) depending on how often requests cross tracks and how lucky rotational alignment is.

#### Bandwidth per track (sectors per unit time)

**Bandwidth** here means sustained sequential read rate on a **single track** (back-to-back sectors, no seek, no extra rotation):

```text
bandwidth = R / zone_angle   (sectors per unit time)
```

At default `-R 1`:

| Track | Zone angle | Transfer per sector | Sequential bandwidth |
|-------|------------|---------------------|----------------------|
| **Outer** | 10° | 10 time units | **1/10 = 0.10** sectors/unit time |
| **Middle** | 20° | 20 time units | **1/20 = 0.05** sectors/unit time |
| **Inner** | 30° | 30 time units | **1/30 ≈ 0.033** sectors/unit time |

**Outer is 3× faster than inner** for sequential reads (0.10 vs 0.033 sectors/unit time).

General formula:

```text
bandwidth_track = R / z_track
```

where `z_track` is the zone angle for that track (10, 20, or 30).

#### Sequential read verification

**Outer track** (`-a 0,1,2,3,4,5`):

```
Block:   1  Seek:  0  Rotate:  0  Transfer: 10  Total:  10
Block:   2  Seek:  0  Rotate:  0  Transfer: 10  Total:  10
...
```

After the first sector, each outer block costs **10** time units → **0.10 sectors/unit time**.

**Middle track** (`-a 36,37,38,39,40,41`):

```
Block:  37  Seek:  0  Rotate:  0  Transfer: 20  Total:  20
...
```

→ **0.05 sectors/unit time**.

**Inner track** (`-a 54,55,56,57,58,59`):

```
Block:  55  Seek:  0  Rotate:  0  Transfer: 30  Total:  30
...
```

→ **≈0.033 sectors/unit time**.

First block on each track still pays seek + rotation to reach the track; bandwidth formula applies to **steady-state sequential** access on that track.

#### Why outer tracks have higher bandwidth

Outer tracks pack **more sectors** into the same 360° (36 vs 12 on inner). Each sector spans fewer degrees, so:

- **Shorter transfer time** per sector (10 vs 30)
- **Higher sequential throughput** (0.10 vs 0.033 sectors/unit time)

This matches real disks: outer zones are used for faster sequential I/O; inner zones have lower linear density and slower per-sector transfer.

#### Summary

| Question | Answer |
|----------|--------|
| `-z 10,20,30` layout | Outer 36 sectors (0–35), middle 18 (36–53), inner 12 (54–65) |
| Random requests | Transfer = zone angle at `-R 1`; rotation usually dominates; totals vary by seed |
| Outer bandwidth | **0.10 sectors/unit time** (= R/10) |
| Middle bandwidth | **0.05 sectors/unit time** (= R/20) |
| Inner bandwidth | **≈0.033 sectors/unit time** (= R/30) |
| Formula | **`bandwidth = R / zone_angle`** |


### 8. A scheduling window determines how many requests the disk can examine at once. Generate random workloads (e.g., -A 1000, -1, 0, with different seeds) and see how long the SATF scheduler takes when the scheduling window is changed from 1 up to the number of requests. How big of a window is needed to maximize performance? Hint: use the -c flag and don’t turn on graphics (-G) to run these quickly. When the scheduling window is set to 1, does it matter which policy you are using?