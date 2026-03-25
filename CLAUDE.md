# CLAUDE.md

This file provides guidance to Claude Code when working with OAI 5G code.

## **IMPORTANT: Downlink Optimization Priority**

**TOP PRIORITY**: 5G NR downlink processing optimization is the highest priority. Focus on:
- ISIP thread pool parallel processing (`openair1/PHY/ISIP_POOL/`)
- Signal generation parallelization (PRS, SSB, CSI-RS)
- Memory initialization optimizations
- Real-time performance for `phy_procedures_gNB_TX()`

## Current Optimization Status

### Implemented Optimizations

| Optimization | File | Description |
|-------------|------|-------------|
| **ISIP Thread Pool** | `isip_pool.c` | 8 worker threads on cores 6,7,8,9,10,11,13,14 |
| **Async Memory Clear** | `phy_procedures_nr_gNB.c` | Memory clear runs in parallel with LDPC encoding |
| **Symbol-Level RE Mapping** | `nr_dlsch.c` | Parallel symbol processing via `re_mapping_symbol_task()` |
| **PMI=0 Fast Path** | `nr_dlsch.c` | Direct layer-to-antenna mapping, skips precoding buffer |
| **DMRS Pre-computation** | `nr_dlsch.c` | DMRS modulation computed before symbol loop |
| **O-RAN Direct txdataF** | `nr_ru_procedures.c` | Skip memcpy - xran reads directly from txdataF |

### Future Optimizations

| Optimization | Status |
|-------------|--------|
| PRS generation parallelization | Ready for implementation |
| SSB generation parallelization | Ready for implementation |
| CSI-RS generation parallelization | Ready for implementation |

### Current Processing Flow (Optimized)

```
+----------------------------------------------------------------+
| PARALLEL SECTION (encoding hides memclear)                      |
|   1. isip_pool_memclear_tx_async() - starts background clear   |
|   2. nr_pdsch_encoding_phase() - CRC + LDPC + rate match        |
|   3. isip_pool_memclear_tx_wait() - wait for memclear          |
|   (encoding ~160us >> memclear ~31us, so wait is ~0us)          |
+----------------------------------------------------------------+
                              |
+----------------------------------------------------------------+
| SEQUENTIAL SECTION (needs clean txdataF)                        |
|   4. PRS -> 5. SSB -> 6. PDCCH                                  |
|   7. PDSCH codeword phase (scrambling, modulation, RE mapping)  |
|      +- Symbol-level parallelization for PMI=0                  |
|   8. CSI-RS -> 9. Phase rotation                                 |
+----------------------------------------------------------------+
```

### Timing Measurement (CSV Fields)

The L1 timing CSV (`l1_slot_timing.csv`) includes:
- `encoding_overlap_ns`: LDPC encoding time (runs parallel with memclear)
- `memclear_wait_ns`: Wait time after encoding (critical path overhead)
- `memory_clear_ns`: Total = encoding_overlap + memclear_wait

Analyze with: `python3 analyze_l1_slot_timing.py`

## Build Commands

### **STANDARD BUILD COMMAND - ALWAYS USE THIS**
```bash
cd cmake_targets/
./build_oai --gNB --ninja -t oran_fhlib_5g --cmake-opt -Dxran_LOCATION=$HOME/RU/phy/fhi_lib/lib -P --build-lib "ldpc_aal"
```

### **STANDARD END TO END TEST COMMAND - ALWAYS USE THIS**

```bash
# With L1 timing enabled (default - generates l1_slot_timing.csv):
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf --thread-pool 1,3 --loader.ldpc.shlibversion _aal --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 --nrLDPC_coding_aal.dpdk_core_list 16-17 --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12 --nrLDPC_coding_t2.eal_init_bbdev 1

# To disable L1 timing (for maximum performance):
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf --thread-pool 1,3 --noenable-l1-timing --loader.ldpc.shlibversion _aal --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 --nrLDPC_coding_aal.dpdk_core_list 16-17 --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12 --nrLDPC_coding_t2.eal_init_bbdev 1
```

### Configuration
- **Current deployment config**: `gnb.sa.band78.273prb.fhi72.4x4-liteon.conf` (ACC100 + Liteon RU)

## Complete Downlink Processing Flow (L3->L2->L1->RU)

### Layer 3 (NGAP/GTP)
**Location**: `openair3/`
- Receives IP packets from core network
- GTP-U encapsulation/decapsulation
- Routes to appropriate SDAP entity

### Layer 2 (MAC/RLC/PDCP/SDAP)
**Location**: `openair2/`

#### SDAP -> PDCP -> RLC -> MAC Flow:
1. **SDAP**: QoS flow to DRB mapping
2. **PDCP**: Header compression, ciphering, integrity protection
3. **RLC**: Segmentation, ARQ, concatenation
4. **MAC** (`openair2/LAYER2/NR_MAC_gNB/`):
   - Scheduling decisions
   - Transport block formation
   - Creates `processingData_L1tx_t` structure

### Layer 1 (PHY) - Main Downlink Entry Point

#### `phy_procedures_gNB_TX()`
**Location**: `openair1/SCHED_NR/phy_procedures_nr_gNB.c`

**Input**: `processingData_L1tx_t *msgTx` from MAC
**Output**: Frequency domain samples in `gNB->common_vars.txdataF[beam][antenna][sample]`

**Signal Generation Pipeline**:
1. **Async Memory Clear**: `isip_pool_memclear_tx_async()` - starts background clear
2. **PDSCH Encoding Phase**: `nr_pdsch_encoding_phase()` - runs parallel with memclear
3. **Wait for Memory Clear**: `isip_pool_memclear_tx_wait()`
4. **PRS Generation**: `nr_generate_prs()`
5. **SSB Processing**: `nr_common_signal_procedures()` (PSS, SSS, PBCH)
6. **PDCCH Generation**: `nr_generate_dci_top()`
7. **PDSCH Codeword Phase**: `nr_pdsch_codeword_phase()` (scrambling, modulation, RE mapping)
8. **CSI-RS Generation**: `nr_generate_csi_rs()`
9. **Phase Rotation**: `apply_nr_rotation_TX()`

### ACC100 Hardware Acceleration

**Software Path**:
```
nr_dlsch_encoding() -> nrLDPC_encoding_segment() -> CPU processing
```

**Hardware Path**:
```
nr_dlsch_encoding() -> gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder() -> ACC100
```

**Configuration**:
```bash
--loader.ldpc.shlibversion _aal
--nrLDPC_coding_aal.dpdk_dev 0000:87:00.0
--nrLDPC_coding_aal.dpdk_core_list 16-17
```

### Radio Unit Processing

#### O-RAN FHI 7.2 Split (Current Deployment)
**DU Processing**:
1. `nr_feptx_prec()`: Skip memcpy - O-RAN reads directly from txdataF
2. `oran_fh_if4p5_south_out()`: O-RAN fronthaul transmission
3. `xran_fh_tx_send_slot()`: Intel xRAN library packetization

**RU Processing** (External Liteon RU):
- Receives O-RAN packets
- OFDM modulation (IFFT + CP)
- RF up-conversion and transmission

## ISIP Thread Pool

### Overview
**Location**: `openair1/PHY/ISIP_POOL/`
- **Architecture**: enkiTS task scheduler with C wrapper
- **Worker Threads**: 8 threads pinned to cores 6,7,8,9,10,11,13,14
- **Scheduling**: Lock-free work-stealing with automatic load balancing

### CMakeLists.txt Setup
**Location**: `openair1/PHY/CMakeLists.txt`
```cmake
# add_subdirectory(NR_THREAD_POOL)  # Disabled
add_subdirectory(ISIP_POOL)         # ISIP thread pool
```

### Integration Points
**Location**: `executables/nr-softmodem.c`
```c
#include "PHY/ISIP_POOL/isip_pool.h"

// Initialization
if (!isip_pool_init()) {
    LOG_W(PHY, "Failed to initialize ISIP thread pool\n");
} else {
    isip_pool_test();
}

// Shutdown
isip_pool_shutdown();
```

### API
```c
// Core functions
int isip_pool_init(void);
void isip_pool_shutdown(void);
int isip_pool_is_initialized(void);
int isip_pool_register_external_thread(void);

// Memory clear operations
void isip_pool_memclear_tx_async(void*** txdataF, int num_beams, int num_antennas,
                                   int offset, int samples_per_slot);
void isip_pool_memclear_tx_wait(void);
```

### Implemented Parallelization

#### 1. Async Memory Clear
**Location**: `phy_procedures_nr_gNB.c` lines 261-290
- Starts memory clear in background via `isip_pool_memclear_tx_async()`
- PDSCH encoding runs in parallel
- Wait completes near-instantly since encoding >> memclear

#### 2. Symbol-Level RE Mapping
**Location**: `nr_dlsch.c` `re_mapping_symbol_task()`
- Processes multiple symbols in parallel (PMI=0 fast path)
- Pre-computes DMRS modulation and RE offsets
- Activates when: PMI=0 + ISIP available + NrOfSymbols >= 4

## Key Data Structures

### `processingData_L1tx_t`
```c
typedef struct processingData_L1tx {
  int frame, slot;
  PHY_VARS_gNB *gNB;
  nfapi_nr_dl_tti_pdcch_pdu pdcch_pdu[];
  NR_gNB_DLSCH_t **dlsch;
  NR_gNB_SSB_t ssb[64];
  NR_gNB_CSIRS_t csirs_pdu[];
  uint16_t num_pdsch_slot;
} processingData_L1tx_t;
```

### Output Format
- **Frequency domain**: `txdataF[beam][antenna][sample]` (complex 16-bit)
- **Samples per slot**: `fp->samples_per_slot_wCP`

## Signal Generation Dependencies

**MUST BE FIRST**: Memory initialization (all signals write to txdataF)

**MUST BE LAST**: Phase rotation (applies to ALL frequency domain samples)

**NO DEPENDENCIES BETWEEN**: PRS, SSB, PDCCH, PDSCH, CSI-RS
- Each writes to different resource elements
- 3GPP 38.211/38.212/38.213 ensure orthogonal resource allocation
- All can potentially run in parallel after memory is cleared
