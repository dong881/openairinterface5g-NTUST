# ACC100 Cross-Slot Encoding Pipeline Implementation Guide

## Overview

This document describes a step-by-step implementation plan for the **cross-slot encoding pipeline** optimization for the OAI 5G NR gNB downlink processing chain. The goal is to overlap ACC100 LDPC encoding for slot N+1 with the codeword processing (scrambling, modulation, RE mapping) for slot N, reducing total slot processing by ~119µs (34%) for 2-layer 273PRB, with even greater benefit for 4-layer MIMO (~209µs, 41%).

### Current vs. Pipeline Timing (2-layer 273PRB)

```
SYNCHRONOUS (current):
Slot N: [MAC ~100µs][Encoding ~160µs][Memclear ~0µs][Signals ~15µs][Codeword ~70µs][PhaseRot ~10µs]
                                                                           total_slot_ns: ~355µs

PIPELINE-HIT:
Slot N: [Collect ~5µs][Memclear ~31µs][Signals ~15µs][Codeword ~70µs][PhaseRot ~10µs][MAC(N+1) ~100µs][Submit ~5µs]
                                                                                        total_slot_ns: ~236µs
```

**Expected improvement**: total_slot_ns reduced from ~355µs to ~236µs (~34% reduction, ~119µs saved).
For 4-layer: ~515µs → ~306µs (~41% reduction, ~209µs saved — critical for meeting 500µs slot budget).

### Key Insight

The ACC100 hardware accelerator performs LDPC encoding asynchronously. Currently, we call `rte_bbdev_enqueue_ldpc_enc_ops()` (submit) and immediately poll `rte_bbdev_dequeue_ldpc_enc_ops()` (collect) in a tight loop, waiting ~193µs. By splitting these two phases across slot boundaries, the ACC100 processes data in the background while we do useful work.

---

## Architecture Overview

### Current Flow (per slot)

```
tx_func(slot N):
  1. MAC scheduling → fills gNB->msgDataTx
  2. phy_procedures_gNB_TX():
     a. enkits_pool_memclear_tx_async()
     b. nr_pdsch_encoding_phase()    ← CRC + Seg + ACC100 submit+collect (~193µs)
     c. enkits_pool_memclear_tx_wait()
     d. PRS/SSB/PDCCH generation
     e. nr_pdsch_codeword_phase()    ← scrambling + modulation + RE mapping (~70µs)
     f. CSI-RS + phase rotation
  3. ru_tx_func_async()
```

### Target Pipeline Flow

```
tx_func(slot N) — Pipeline HIT (slots 1,2 of DDDSU):
  1. SKIP NR_slot_indication (scheduling already done in slot N-1's pipeline)
  2. Collect ACC100 results from pipeline (~5µs)
  3. phy_procedures_gNB_TX(enc_buf_idx >= 0):
     a. enkits_pool_memclear_tx_async() + wait (~31µs, no parallel encoding)
     b. PRS/SSB/PDCCH generation
     c. nr_pdsch_codeword_phase(slot N)    ← uses pre-encoded data
     d. CSI-RS + phase rotation
  4. Pipeline for N+1:
     a. NR_slot_indication(N+1) — MAC scheduling for next slot
     b. CRC + Segmentation + ACC100 submit (~5µs, non-blocking)
  5. ru_tx_func_async()

tx_func(slot N) — Pipeline MISS (slot 0 of DDDSU, first DL after UL gap):
  1. NR_slot_indication(N) — normal MAC scheduling
  2. phy_procedures_gNB_TX(enc_buf_idx = -1):
     a. enkits_pool_memclear_tx_async()
     b. nr_pdsch_encoding_phase() — full synchronous encoding (~160µs, hides memclear)
     c. PRS/SSB/PDCCH, Codeword, CSI-RS, phase rotation
  3. Pipeline for N+1 (same as above step 4)
  4. ru_tx_func_async()
```

### Data Flow Diagram

```
Slot N-1                    Slot N (pipeline hit)       Slot N+1 (pipeline hit)
─────────────────────────┬──────────────────────────┬───────────────────────
 ... Submit(N) to ACC100 │ Collect(N) from ACC100   │ Collect(N+1) from ACC100
                         │ [MAC(N) skipped]         │ [MAC(N+1) skipped]
                         │ Codeword(N) → txdataF    │ Codeword(N+1) → txdataF
                         │ MAC sched(N+1)           │ MAC sched(N+2)
                         │ CRC+Seg(N+1)             │ CRC+Seg(N+2)
                         │ Submit(N+1) to ACC100    │ Submit(N+2) to ACC100
─────────────────────────┴──────────────────────────┴───────────────────────
                               ↑ ACC100 processes N+1 in background
```

---

## Data Dependency Safety Analysis

Before implementing, we must verify that cross-slot data dependencies are safe:

| Data Structure | How Used | Cross-Slot Safety |
|---|---|---|
| `harq->pdsch_pdu` | `memcpy()` in `nr_fill_dlsch_dl_tti_req()` (nr_dlsch_tools.c:42) | **SAFE** - Deep copy, independent across slots |
| `harq->pdu` (SDU pointer) | Points into `sched_response` buffer (nr_dlsch_tools.c:61) | **SAFE** - Only read during CRC+Segmentation, data is copied into `harq->c[r]` before ACC100 |
| `harq->c[r]` (segment buffers) | Pre-allocated per HARQ process (8448 bytes each) | **SAFE** - Data is `rte_memcpy()`'d into DPDK mbufs by `init_op_data_objs_enc()` (nrLDPC_coding_aal.c:541/554). After mbuf allocation, `harq->c[r]` is free |
| `g_pdsch_encoded_output` | Global 512KB static buffer | **UNSAFE** - Must be double-buffered |
| `gNB->msgDataTx` | Single `processingData_L1tx_t` pointer | **UNSAFE** - Must become dual-buffer |
| `sched_response` pool | N_RESP=3 with reference counting | **SAFE** - Supports 2 concurrent responses. Pipeline needs at most 2 |
| ACC100 enc_queue | Separate from dec_queue (nrLDPC_coding_aal.c:412,426) | **SAFE** - Encoding doesn't block decoding |
| VLA `TBs[]` / `segments[]` | Stack-allocated in `nr_dlsch_encoding()` (nr_dlsch_coding.c:131,146) | **UNSAFE** - Must persist across slot boundary for collect phase |

---

## Implementation Phases

### Phase 1: Split-Phase API + Double-Buffer Infrastructure (merged)

> **Design Note**: The original plan split this into two phases (ACC100 API and buffers), but
> code review revealed they are **inseparable** — the split-phase API's `collect` calls
> `retrieve_ldpc_enc_op()` which dereferences `ctx->params->TBs[h].output` and
> `ctx->params->TBs[h].segments[r].E` (nrLDPC_coding_aal.c:788-821). These params are
> currently stack-allocated VLAs in `nr_dlsch_encoding()` (nr_dlsch_coding.c:131,146) and
> would be dangling pointers if collect runs in a different slot than submit. Therefore,
> persistent encoding parameter storage **must** be implemented together with the split-phase
> API. The two are merged here as a single atomically verifiable phase.

**Goal**: (1) Split the synchronous ACC100 encoding into submit/collect phases, and
(2) create the persistent buffer infrastructure needed for cross-slot operation.

**Files to modify**:
- `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h`
- `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface_load.c`
- `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_aal/nrLDPC_coding_aal.c`
- `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`
- `openair1/PHY/NR_TRANSPORT/nr_dlsch_coding.c`
- `openair1/SCHED_NR/phy_procedures_nr_gNB.c`

---

#### Step 1.1: Define new interface types in nrLDPC_coding_interface.h

Add an opaque handle and new function pointer typedefs. Note: The interface struct gains
two **optional** function pointers (may be NULL for non-AAL backends like the software
encoder `libldpc.so` or xDMA `libldpc_xdma.so`).

```c
// After line 248 (nrLDPC_coding_encoder_t typedef)

/** Opaque handle for in-flight encoding operation */
typedef struct nrLDPC_encoding_context_s nrLDPC_encoding_context_t;

/**
 * \brief Submit encoding to hardware, returns immediately
 * \param params encoding parameters (must remain valid until collect)
 * \param ctx output: opaque context handle for collect phase
 * \return 0 on success
 */
typedef int32_t(nrLDPC_coding_encoder_submit_t)(
    nrLDPC_slot_encoding_parameters_t *params,
    nrLDPC_encoding_context_t **ctx);

/**
 * \brief Collect encoding results from a previous submit
 * \param ctx context handle from submit phase (freed after collect)
 * \return 0 on success
 */
typedef int32_t(nrLDPC_coding_encoder_collect_t)(
    nrLDPC_encoding_context_t *ctx);
```

Extend the interface struct:

```c
typedef struct nrLDPC_coding_interface_s {
  nrLDPC_coding_init_t *nrLDPC_coding_init;
  nrLDPC_coding_shutdown_t *nrLDPC_coding_shutdown;
  nrLDPC_coding_decoder_t *nrLDPC_coding_decoder;
  nrLDPC_coding_encoder_t *nrLDPC_coding_encoder;           // existing synchronous API
  nrLDPC_coding_encoder_submit_t *nrLDPC_coding_encoder_submit;   // NEW (may be NULL)
  nrLDPC_coding_encoder_collect_t *nrLDPC_coding_encoder_collect; // NEW (may be NULL)
} nrLDPC_coding_interface_t;
```

#### Step 1.2: Update loader for optional symbols

**Critical Issue**: The OAI loader (`load_module_version_shlib()` in
`common/utils/load_module_shlib.c:205`) calls `dlsym()` for each entry in the
`shlib_fdesc[]` array and **fatally exits** if any symbol is not found. The software
encoder (`libldpc.so`) and xDMA backend (`libldpc_xdma.so`) will never export
`nrLDPC_coding_encoder_submit` / `nrLDPC_coding_encoder_collect`, so we **cannot**
add them to the mandatory symbol array.

**Solution**: In `nrLDPC_coding_interface_load.c`, resolve the 4 mandatory symbols
as before, then **manually** attempt optional resolution via `dlsym()` with the
already-loaded library handle:

```c
int load_nrLDPC_coding_interface(char *version, nrLDPC_coding_interface_t *itf)
{
  // ... existing code: load 4 mandatory symbols via shlib_fdesc[] ...

  itf->nrLDPC_coding_init = (nrLDPC_coding_init_t *)shlib_fdesc[0].fptr;
  itf->nrLDPC_coding_shutdown = (nrLDPC_coding_shutdown_t *)shlib_fdesc[1].fptr;
  itf->nrLDPC_coding_decoder = (nrLDPC_coding_decoder_t *)shlib_fdesc[2].fptr;
  itf->nrLDPC_coding_encoder = (nrLDPC_coding_encoder_t *)shlib_fdesc[3].fptr;

  // NEW: Attempt optional pipeline symbols (NULL if not available)
  // Use RTLD_DEFAULT to search all loaded shared libraries
  itf->nrLDPC_coding_encoder_submit =
      (nrLDPC_coding_encoder_submit_t *)dlsym(RTLD_DEFAULT, "nrLDPC_coding_encoder_submit");
  itf->nrLDPC_coding_encoder_collect =
      (nrLDPC_coding_encoder_collect_t *)dlsym(RTLD_DEFAULT, "nrLDPC_coding_encoder_collect");

  if (itf->nrLDPC_coding_encoder_submit && itf->nrLDPC_coding_encoder_collect) {
    LOG_I(PHY, "LDPC pipeline API available (split-phase submit/collect)\n");
  } else {
    LOG_I(PHY, "LDPC pipeline API not available (synchronous encoding only)\n");
    itf->nrLDPC_coding_encoder_submit = NULL;
    itf->nrLDPC_coding_encoder_collect = NULL;
  }

  AssertFatal(itf->nrLDPC_coding_init() == 0, "error starting LDPC library\n");
  return 0;
}
```

This works because `load_module_version_shlib()` opens the library with
`RTLD_GLOBAL` (load_module_shlib.c:160), so symbols are visible via `RTLD_DEFAULT`.
Both submit and collect must be present or both NULL — partial availability is treated
as unavailable.

#### Step 1.3: Implement the context structure in AAL

In `nrLDPC_coding_aal.c`, define the context that holds all state between submit and collect:

```c
// After the existing struct definitions (after struct data_buffers, line ~78)

struct nrLDPC_encoding_context_s {
  // ACC100 device state
  struct rte_bbdev_enc_op *ops_enq[MAX_BURST];  // MAX_BURST=512 (nrLDPC_coding_aal.h:15)
  struct rte_bbdev_enc_op *ops_deq[MAX_BURST];
  uint16_t num_segments;
  uint16_t enqueued;
  uint16_t dequeued;

  // Device IDs (copied from active_dev at submit time)
  uint8_t dev_id;
  uint16_t queue_id;

  // DPDK mbuf references (for cleanup in collect)
  struct rte_bbdev_op_data *queue_ops_input;
  struct rte_bbdev_op_data *queue_ops_output;

  // Encoding parameters (pointer — caller guarantees lifetime via persistent storage)
  nrLDPC_slot_encoding_parameters_t *params;
};
```

> **Note on `data_buffers` struct**: The original proposal stored a
> `struct data_buffers` in the context. This is unnecessary — we only need
> `queue_ops_input` and `queue_ops_output` pointers for mbuf cleanup.
> The `data_buffers.harq_outputs` field is unused for encoding.

#### Step 1.4: Implement `nrLDPC_coding_encoder_submit()` (AAL exported symbol)

This function performs everything up to and including `rte_bbdev_enqueue_ldpc_enc_ops()`,
but does NOT poll for completion. It must be a **non-static exported symbol** (no `static`
keyword) so `dlsym()` can find it.

```c
// EXPORTED — must not be static
int32_t nrLDPC_coding_encoder_submit(
    nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters,
    nrLDPC_encoding_context_t **ctx_out)
{
  pthread_mutex_lock(&encode_mutex);
  // NOTE: mutex stays locked until collect — see safety discussion below

  if (nrLDPC_slot_encoding_parameters->tprep != NULL)
    start_meas(nrLDPC_slot_encoding_parameters->tprep);

  const uint16_t num_segments = nb_segments_encoding(nrLDPC_slot_encoding_parameters);

  // Allocate context on DPDK hugepage memory (cache-line aligned)
  nrLDPC_encoding_context_t *ctx = rte_zmalloc(NULL, sizeof(*ctx), RTE_CACHE_LINE_SIZE);
  AssertFatal(ctx != NULL, "Failed to allocate encoding context");

  ctx->num_segments = num_segments;
  ctx->dev_id = active_dev.dev_id;
  ctx->queue_id = active_dev.enc_queue;
  ctx->params = nrLDPC_slot_encoding_parameters;

  int socket_id = active_dev.info.socket_id;

  // Allocate mbuf buffers (mirrors nrLDPC_coding_encoder lines 1360-1373)
  struct rte_mempool *mbuf_pools[2] = {active_dev.in_mbuf_pool, active_dev.hard_out_mbuf_pool};
  struct rte_bbdev_op_data **queue_ops[2] = {&ctx->queue_ops_input, &ctx->queue_ops_output};

  for (enum op_data_type type = DATA_INPUT; type < 2; ++type) {
    int ret = allocate_buffers_on_socket(queue_ops[type],
                                          num_segments * sizeof(struct rte_bbdev_op_data), socket_id);
    AssertFatal(ret == 0, "Couldn't allocate memory for rte_bbdev_op_data structs");
    ret = init_op_data_objs_enc(*queue_ops[type], nrLDPC_slot_encoding_parameters,
                                 mbuf_pools[type], type, active_dev.info.drv.min_alignment);
    AssertFatal(ret == 0, "Couldn't init rte_bbdev_op_data structs");
  }
  // After init_op_data_objs_enc() returns, segment data (harq->c[r]) has been
  // rte_memcpy'd into DPDK mbufs (nrLDPC_coding_aal.c:541/554). The original
  // harq->c[r] buffers are no longer needed by ACC100.

  // Allocate bbdev ops and set parameters
  int ret = rte_bbdev_enc_op_alloc_bulk(active_dev.bbdev_enc_op_pool, ctx->ops_enq, num_segments);
  AssertFatal(ret == 0, "Allocation failed for %d ops", num_segments);
  set_ldpc_enc_op(ctx->ops_enq, ctx->queue_ops_input, ctx->queue_ops_output,
                   nrLDPC_slot_encoding_parameters);

  if (nrLDPC_slot_encoding_parameters->tprep != NULL)
    stop_meas(nrLDPC_slot_encoding_parameters->tprep);
  if (nrLDPC_slot_encoding_parameters->tparity != NULL)
    start_meas(nrLDPC_slot_encoding_parameters->tparity);

  // Enqueue ALL operations to ACC100 hardware
  ctx->enqueued = 0;
  ctx->dequeued = 0;
  while (ctx->enqueued < num_segments) {
    uint16_t num_to_enq = num_segments - ctx->enqueued;
    ctx->enqueued += rte_bbdev_enqueue_ldpc_enc_ops(ctx->dev_id, ctx->queue_id,
                                                     &ctx->ops_enq[ctx->enqueued], num_to_enq);
    // Opportunistically dequeue any already-complete ops
    ctx->dequeued += rte_bbdev_dequeue_ldpc_enc_ops(ctx->dev_id, ctx->queue_id,
                                                     &ctx->ops_deq[ctx->dequeued],
                                                     ctx->enqueued - ctx->dequeued);
  }

  *ctx_out = ctx;
  // encode_mutex remains locked — unlocked in collect
  return 0;
}
```

#### Step 1.5: Implement `nrLDPC_coding_encoder_collect()` (AAL exported symbol)

```c
// EXPORTED — must not be static
int32_t nrLDPC_coding_encoder_collect(nrLDPC_encoding_context_t *ctx)
{
  AssertFatal(ctx != NULL, "nrLDPC_coding_encoder_collect called with NULL context");

  // Dequeue remaining operations (may be 0 if all completed during submit)
  int time_out = 0;
  while (ctx->dequeued < ctx->enqueued) {
    ctx->dequeued += rte_bbdev_dequeue_ldpc_enc_ops(ctx->dev_id, ctx->queue_id,
                                                     &ctx->ops_deq[ctx->dequeued],
                                                     ctx->enqueued - ctx->dequeued);
    time_out++;
    if (time_out > TIME_OUT_POLL) {
      LOG_E(PHY, "ACC100 encoding collect timeout after %d polls\n", time_out);
      // MUST still free resources and unlock mutex even on error
      goto cleanup;
    }
  }

  if (ctx->params->tparity != NULL)
    stop_meas(ctx->params->tparity);
  if (ctx->params->toutput != NULL)
    start_meas(ctx->params->toutput);

  // Copy results from output mbufs to the persistent output buffer
  // retrieve_ldpc_enc_op reads: ctx->params->TBs[h].output, .C, .segments[r].E
  int ret = retrieve_ldpc_enc_op(ctx->ops_deq, ctx->params);
  AssertFatal(ret == 0, "Failed to retrieve LDPC encoding op!");

  if (ctx->params->toutput != NULL)
    stop_meas(ctx->params->toutput);

cleanup:
  // Free bbdev ops (returns to bbdev_enc_op_pool)
  rte_bbdev_enc_op_free_bulk(ctx->ops_enq, ctx->num_segments);

  // Free mbufs (returns to in_mbuf_pool and hard_out_mbuf_pool)
  for (int segment = 0; segment < ctx->num_segments; ++segment) {
    rte_pktmbuf_free(ctx->queue_ops_input[segment].data);
    rte_pktmbuf_free(ctx->queue_ops_output[segment].data);
  }
  rte_free(ctx->queue_ops_input);
  rte_free(ctx->queue_ops_output);

  // Free context itself
  rte_free(ctx);

  // Unlock the encode mutex (locked in submit)
  pthread_mutex_unlock(&encode_mutex);

  return (time_out > TIME_OUT_POLL) ? -1 : 0;
}
```

> **Safety: `encode_mutex` held across slots**
>
> The `encode_mutex` (nrLDPC_coding_aal.c:47) is locked in submit and unlocked in collect,
> potentially spanning ~500µs across a slot boundary. This is safe because:
> - `decode_mutex` is separate (line 48) — UL decoding is not blocked
> - `enc_queue` and `dec_queue` are separate hardware queues (lines 412, 426)
> - Only one TX thread calls encoding (L1_tx_thread)
> - The collect function has a cleanup-on-error path that **always** unlocks the mutex,
>   even on ACC100 timeout, preventing deadlock

#### Step 1.6: Double-buffer `g_pdsch_encoded_output`

In `nr_dlsch.c`, replace the single static buffer (line 241-243):

```c
// BEFORE:
// static unsigned char __attribute__((aligned(64))) g_pdsch_encoded_output[PDSCH_ENCODED_OUTPUT_MAX_SIZE];
// static size_t g_pdsch_encoded_size = 0;

// AFTER:
#define NUM_ENCODED_BUFFERS 2
static unsigned char __attribute__((aligned(64)))
    g_pdsch_encoded_output[NUM_ENCODED_BUFFERS][PDSCH_ENCODED_OUTPUT_MAX_SIZE];
static size_t g_pdsch_encoded_size[NUM_ENCODED_BUFFERS] = {0, 0};
```

> **Memory impact**: 2 × 512KB = 1MB total. Negligible for a gNB system.

#### Step 1.7: Persistent encoding parameter storage

In `nr_dlsch_coding.c`, the VLA arrays on the stack (line 131: `TBs[msgTx->num_pdsch_slot]`,
line 146: `segments[MAX_SEGMENTS_PER_SLOT]`) and the local `slot_parameters` struct
(line 133) are destroyed when `nr_dlsch_encoding()` returns. For the pipeline, `collect`
calls `retrieve_ldpc_enc_op()` which dereferences these via `ctx->params`. They must
therefore persist from submit until after collect completes.

Add persistent storage at file scope in `nr_dlsch_coding.c`:

```c
// MAX_SEGMENTS_PER_SLOT is already defined at line 145:
// #define MAX_SEGMENTS_PER_SLOT (MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * NR_MAX_NB_LAYERS * 8)
// = 36 * 4 * 8 = 1152 (worst case; typical 4-layer 273PRB ≈ 52 segments)

#define MAX_TBS_PER_SLOT 8  // max PDSCHs scheduled in one slot

typedef struct {
  nrLDPC_TB_encoding_parameters_t TBs[MAX_TBS_PER_SLOT];
  nrLDPC_segment_encoding_parameters_t segments[MAX_SEGMENTS_PER_SLOT];
  nrLDPC_slot_encoding_parameters_t slot_params;
  int num_pdsch;         // how many TBs are valid
  bool in_use;           // guard against double-use
} persistent_encoding_params_t;

static persistent_encoding_params_t g_enc_params[NUM_ENCODED_BUFFERS];
```

> **Size estimate**: `nrLDPC_segment_encoding_parameters_t` contains 1 int + 1 pointer +
> 3 × `time_stats_t`. `time_stats_t` is ~96 bytes (time_meas.h:59-72). So each segment
> param ≈ 300 bytes. 1152 segments × 300B ≈ 337KB per buffer, 674KB total for two.
> Plus `TBs`: 8 × ~80B ≈ 640B. Total ≈ 675KB — acceptable for a static allocation.
>
> **Alternative**: Use a smaller `MAX_SEGMENTS_PER_SLOT` (e.g., 256 for 4-layer 273PRB
> max ~52 segments with margin). This reduces to ~150KB total.

#### Step 1.8: Update `nr_pdsch_encoding_phase()` to accept buffer index

Modify `nr_pdsch_encoding_phase()` in `nr_dlsch.c` (line 1518):

```c
int nr_pdsch_encoding_phase(processingData_L1tx_t *msgTx, int frame, int slot, int buf_idx)
{
  // ... existing size calculation (lines 1534-1557 unchanged) ...

  // Use indexed buffer
  g_pdsch_encoded_size[buf_idx] = size_output;
  bzero(g_pdsch_encoded_output[buf_idx], size_output >> 3);

  // Pass indexed buffer to encoding
  if (nr_dlsch_encoding(gNB, msgTx, frame, slot, frame_parms,
                        g_pdsch_encoded_output[buf_idx],  // <-- indexed
                        tinput, tprep, tparity, toutput,
                        dlsch_rate_matching_stats, dlsch_interleaving_stats,
                        dlsch_segmentation_stats) == -1) {
    return -1;
  }
  // ...
}
```

#### Step 1.9: Update `nr_pdsch_codeword_phase()` to accept buffer index

```c
void nr_pdsch_codeword_phase(processingData_L1tx_t *msgTx, int frame, int slot, int buf_idx)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;
  LOG_D(PHY, "PDSCH codeword phase started (%d) in frame %d.%d\n", msgTx->num_pdsch_slot, frame, slot);

  // Use indexed buffer
  unsigned char *output_ptr = g_pdsch_encoded_output[buf_idx];
  for (int dlsch_id = 0; dlsch_id < msgTx->num_pdsch_slot; dlsch_id++) {
    output_ptr += do_one_dlsch(output_ptr, gNB, msgTx->dlsch[dlsch_id], slot, dlsch_id);
  }
}
```

#### Step 1.10: Update all callers in `phy_procedures_nr_gNB.c`

For this phase, always use `buf_idx = 0` (no toggling yet — toggling comes in Phase 2/3
when dual msgDataTx and pipeline state machine are active):

```c
// In phy_procedures_gNB_TX():
if (pdsch_count > 0) {
    nr_pdsch_encoding_phase(msgTx, frame, slot, 0);  // always buf_idx=0 for now
}
// ...
if (pdsch_count > 0) {
    nr_pdsch_codeword_phase(msgTx, frame, slot, 0);  // always buf_idx=0 for now
}
```

#### Step 1.11: Verify mbuf pool sizing for pipeline use

**Concern**: In pipeline mode, submit holds mbufs until collect (across a slot boundary).
If a second encoding happens before collect (shouldn't in our design, but defense-in-depth):

Current pool size: `mbuf_pool_size = optimal_mempool_size(ops_pool_size * 1)` where
`ops_pool_size ≈ 511`. A 4-layer 273PRB slot has ~52 segments × 2 mbufs (input+output)
= ~104 mbufs per encoding. Pool of 511 supports holding one full set (104) while the pool
is available for decoding.

**Verification**: Add a runtime check in submit:
```c
LOG_D(PHY, "Pipeline submit: %d segments, mbuf pool avail in=%u out=%u\n",
      num_segments,
      rte_mempool_avail_count(active_dev.in_mbuf_pool),
      rte_mempool_avail_count(active_dev.hard_out_mbuf_pool));
```

If pool exhaustion occurs in testing, increase `num_ops` from 1 to 2 in
`create_mempools()` (nrLDPC_coding_aal.c:135).

---

#### Verification (Phase 1)

This is the **first checkpoint** — all changes above must be verified together:

1. **Build test**: Compile with both AAL and software encoder:
   ```bash
   cd cmake_targets/
   # Build with AAL
   ./build_oai --gNB --ninja -t oran_fhlib_5g \
     --cmake-opt -Dxran_LOCATION=$HOME/RU/phy/fhi_lib/lib -P --build-lib "ldpc_aal"
   # Also verify software encoder builds (no new mandatory symbols)
   ./build_oai --gNB --ninja
   ```

2. **Software encoder regression**: Run with default `libldpc.so` (no `--loader.ldpc.shlibversion`):
   - Must not crash — `submit`/`collect` should be NULL
   - Verify log message: `"LDPC pipeline API not available"`

3. **AAL regression**: Run standard end-to-end test with AAL:
   ```bash
   sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.273prb.fhi72.4x4-liteon.conf \
     --thread-pool 1,3 --loader.ldpc.shlibversion _aal \
     --nrLDPC_coding_aal.dpdk_dev 0000:87:00.0 --nrLDPC_coding_aal.dpdk_core_list 16-17 \
     --nrLDPC_coding_aal.vfio_vf_token c2d9f0a2-bc24-4a83-8126-9fbb22f3ce12
   ```
   - Must work identically to before (synchronous path, `buf_idx=0`)
   - Verify log message: `"LDPC pipeline API available"`

4. **Submit→Collect equivalence test**: Add a temporary test in `nr_dlsch_encoding()` that
   exercises the new API and verifies bit-exact output:
   ```c
   #ifdef PIPELINE_SELF_TEST
   // After synchronous encoding completes, verify submit+collect produces same output
   unsigned char test_output[PDSCH_ENCODED_OUTPUT_MAX_SIZE];
   memcpy(test_output, output, size_output >> 3);  // save synchronous result

   // Setup persistent params for test
   persistent_encoding_params_t test_persist;
   // ... copy TBs/segments into test_persist, point output to test_output2 ...

   nrLDPC_encoding_context_t *ctx;
   gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder_submit(&test_persist.slot_params, &ctx);
   gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder_collect(ctx);

   // Compare: test_output vs test_output2 must be identical
   AssertFatal(memcmp(test_output, test_output2, size_output >> 3) == 0,
               "Pipeline self-test FAILED: output mismatch!");
   LOG_I(PHY, "Pipeline self-test PASSED\n");
   #endif
   ```

5. **Timing test**: Check `l1_slot_timing.csv` — timing should be unchanged (within noise)
   since we still use the synchronous path

---

### Phase 2: Dual msgDataTx Buffers

**Goal**: Create two `processingData_L1tx_t` structures so slot N's codeword phase can read from one while slot N+1's MAC scheduling fills the other.

**Files to modify**:
- `openair1/PHY/defs_gNB.h`
- `executables/nr-gnb.c`
- `openair1/SCHED_NR/fapi_nr_l1.c`

#### Step 2.1: Change msgDataTx to array in defs_gNB.h

```c
// BEFORE (line 554):
// struct processingData_L1tx *msgDataTx;

// AFTER:
#define NUM_TX_BUFFERS 2
struct processingData_L1tx *msgDataTx[NUM_TX_BUFFERS];
int msgDataTx_idx;  // current buffer index (0 or 1)
```

#### Step 2.2: Allocate both buffers in init_gNB_Tpool()

In `nr-gnb.c` (around line 391):

```c
// BEFORE:
// notifiedFIFO_elt_t *msgL1Tx = newNotifiedFIFO_elt(sizeof(processingData_L1tx_t), 0, &gNB->L1_tx_out, NULL);
// processingData_L1tx_t *msgDataTx = (processingData_L1tx_t *)NotifiedFifoData(msgL1Tx);
// ...
// gNB->msgDataTx = msgDataTx;

// AFTER:
for (int i = 0; i < NUM_TX_BUFFERS; i++) {
  notifiedFIFO_elt_t *msgL1Tx = newNotifiedFIFO_elt(sizeof(processingData_L1tx_t), 0, &gNB->L1_tx_out, NULL);
  processingData_L1tx_t *msgDataTx = (processingData_L1tx_t *)NotifiedFifoData(msgL1Tx);
  memset(msgDataTx, 0, sizeof(processingData_L1tx_t));
  init_DLSCH_struct(gNB, msgDataTx);
  memset(msgDataTx->ssb, 0, 64 * sizeof(NR_gNB_SSB_t));
  gNB->msgDataTx[i] = msgDataTx;
}
gNB->msgDataTx_idx = 0;
```

**IMPORTANT**: The `newNotifiedFIFO_elt()` call allocates memory for the FIFO element, and `NotifiedFifoData()` returns the data portion. We need TWO separate FIFO elements. However, with the pipeline approach, only one msgDataTx is active in `tx_func` at a time (the FIFO mechanism changes — see Phase 3 notes).

#### Step 2.3: Update all `gNB->msgDataTx` references in fapi_nr_l1.c

There are **4** references that must be updated (all in `fapi_nr_l1.c`):

```c
// nr_schedule_dl_tti_req() line 89:
processingData_L1tx_t *msgTx = gNB->msgDataTx[gNB->msgDataTx_idx];

// nr_schedule_tx_req() line 178:
processingData_L1tx_t *msgTx = gNB->msgDataTx[gNB->msgDataTx_idx];

// nr_schedule_ul_dci_req() line 190:
processingData_L1tx_t *msgTx = gNB->msgDataTx[gNB->msgDataTx_idx];

// nr_schedule_response() line 218:
processingData_L1tx_t *msgTx = gNB->msgDataTx[gNB->msgDataTx_idx];
```

> **Note**: All 4 functions are called from `nr_schedule_response()` within the
> same call chain, so they all use the same `msgDataTx_idx` value — no race condition.

#### Step 2.4: Update tx_func() references

In `nr-gnb.c`, update the `tx_func()` (line 120-121):

```c
// BEFORE:
// gNB->msgDataTx->timestamp_tx = info->timestamp_tx;
// info = gNB->msgDataTx;

// AFTER:
gNB->msgDataTx[gNB->msgDataTx_idx]->timestamp_tx = info->timestamp_tx;
info = gNB->msgDataTx[gNB->msgDataTx_idx];
```

#### Step 2.5: Update num_pdsch_slot reset

Currently, `num_pdsch_slot` is reset implicitly when `nr_fill_dlsch_dl_tti_req()` starts filling (it starts at 0 from `memset` during init). In the pipeline, each buffer must be reset before MAC scheduling fills it.

In `nr_schedule_response()` (`fapi_nr_l1.c`), before the DL scheduling:

```c
processingData_L1tx_t *msgTx = gNB->msgDataTx[gNB->msgDataTx_idx];
msgTx->num_pdsch_slot = 0;  // Reset PDSCH count for this buffer
msgTx->num_dl_pdcch = 0;
msgTx->num_ul_pdcch = 0;
```

#### Step 2.6: Update term_gNB_Tpool cleanup

Update cleanup to free both buffers (if needed).

#### Step 2.7: Update simulator references

In `openair1/SIMULATION/NR_PHY/dlsim.c` (line 982) and `ulsim.c` (line 616), update
`gNB->msgDataTx = msgDataTx` to `gNB->msgDataTx[0] = msgDataTx`.

> **Complete reference list** (verified via `grep -rn "gNB->msgDataTx" --include='*.c'`):
>
> | File | Line | Function | Change |
> |---|---|---|---|
> | `executables/nr-gnb.c` | 120-121 | `tx_func()` | `→ msgDataTx[msgDataTx_idx]` |
> | `executables/nr-gnb.c` | 397 | `init_gNB_Tpool()` | `→ msgDataTx[i]` in loop |
> | `openair1/SCHED_NR/fapi_nr_l1.c` | 89 | `nr_schedule_dl_tti_req()` | `→ msgDataTx[msgDataTx_idx]` |
> | `openair1/SCHED_NR/fapi_nr_l1.c` | 178 | `nr_schedule_tx_req()` | `→ msgDataTx[msgDataTx_idx]` |
> | `openair1/SCHED_NR/fapi_nr_l1.c` | 190 | `nr_schedule_ul_dci_req()` | `→ msgDataTx[msgDataTx_idx]` |
> | `openair1/SCHED_NR/fapi_nr_l1.c` | 218 | `nr_schedule_response()` | `→ msgDataTx[msgDataTx_idx]` |
> | `openair1/SIMULATION/NR_PHY/dlsim.c` | 982 | main | `→ msgDataTx[0]` |
> | `openair1/SIMULATION/NR_PHY/ulsim.c` | 616 | main | `→ msgDataTx[0]` |
>
> **Memory impact**: Each `processingData_L1tx_t` contains `dlsch` pointer array
> (16 entries × `NR_gNB_DLSCH_t` with HARQ buffers: ~16 × 144 segments × 8448B ≈ 19MB).
> Doubling means ~19MB extra. Acceptable for a gNB server.

#### Verification (Phase 2)

1. **Build test**: Compile cleanly (including dlsim/ulsim)
   ```bash
   cd cmake_targets/
   ./build_oai --gNB --ninja -t oran_fhlib_5g \
     --cmake-opt -Dxran_LOCATION=$HOME/RU/phy/fhi_lib/lib -P --build-lib "ldpc_aal"
   ```
2. **Regression test**: Run end-to-end test — only `msgDataTx[0]` is used
   (`msgDataTx_idx` never changes from 0), behavior must be identical
3. **Memory check**: Add temporary LOG at startup verifying both buffers are allocated:
   ```c
   LOG_I(PHY, "Dual msgDataTx allocated: [0]=%p dlsch=%p, [1]=%p dlsch=%p\n",
         gNB->msgDataTx[0], gNB->msgDataTx[0]->dlsch,
         gNB->msgDataTx[1], gNB->msgDataTx[1]->dlsch);
   ```
4. **Simulator test**: Run `dlsim` and `ulsim` to verify they work with `msgDataTx[0]`

---

### Phase 3: Pipeline State Machine in tx_func

**Goal**: Implement the core pipeline logic that overlaps encoding submit/collect across slot boundaries.

**Files to modify**:
- `executables/nr-gnb.c`
- `openair1/SCHED_NR/phy_procedures_nr_gNB.c`
- `openair1/PHY/NR_TRANSPORT/nr_dlsch_coding.c`
- `openair1/PHY/NR_TRANSPORT/nr_dlsch.c`

#### Critical Design Issues Found During Code Cross-Reference

Three critical issues were discovered in the original Phase 3 design by cross-referencing with
the actual `tx_func()` (`nr-gnb.c:78-186`), `run_scheduler_monolithic()` (`NR_IF_Module.c:390-425`),
`nr_schedule_response()` (`fapi_nr_l1.c:197-248`), `nr_sched_response.c` (N_RESP=3 pool),
and `gNB_dlsch_ulsch_scheduler()` (`gNB_scheduler.c:163-271`).

**Issue 1 (SEVERE): sched_response pool exhaustion**

The original design calls `NR_slot_indication(N+1)` during slot N for pipeline scheduling.
This internally calls `allocate_sched_response()` (refcount=1) → `nr_schedule_response()` which
does `inc_ref(+1=2)` then `deref(-1=1)` for DL slots. The pipeline's sched_response (refcount=1)
is stored in `msgDataTx[next_buf]->sched_response_id`.

However, at the end of `tx_func`, only the CURRENT slot's sched_response is released via
`deref_sched_response(info->sched_response_id)`. The pipeline's sched_response is **never deref'd**.
With `N_RESP=3`, the pool exhausts after 3 pipeline slots → `exit(1)` at `nr_sched_response.c:89`.

**Issue 2 (SEVERE): Double MAC scheduling produces mismatched encoding/codeword data**

The original design calls `NR_slot_indication(N+1)` during slot N (pipeline), then calls
`NR_slot_indication(N+1)` AGAIN at the start of slot N+1's tx_func (normal flow). The second
call may produce different scheduling results (HARQ feedback arrived, CQI changed, buffer status
updated between slots). This causes a fatal mismatch:
- LDPC encoding was done with first call's parameters (TBS, code rate, RB allocation)
- Codeword phase (scrambling, modulation, RE mapping) uses second call's parameters
- If parameters differ → corrupted transmission, CRC failure at UE

**Issue 3 (MEDIUM): MAC scheduler double-invocation corrupts internal state**

`gNB_dlsch_ulsch_scheduler()` (`gNB_scheduler.c:163-271`) has extensive side effects:
- `nr_mac_update_timers()` (line 205): advances HARQ/BSR/PHR timers
- VRB map clearing (lines 184-194): resets resource allocation maps
- `schedule_nr_prach()` (line 230): PRACH resource management
- `nr_schedule_RA()` (line 244): RA procedure state machine
- `nr_schedule_ulsch()` (line 249): UL grant allocation

Calling this twice for the same slot would double-advance timers, clear VRB maps filled by
the first call, and potentially double-allocate HARQ processes or RA responses.

**Solution: Skip NR_slot_indication when pipeline data is available**

All three issues are resolved by a single design change: when the pipeline already scheduled
slot N+1 during slot N, **skip `NR_slot_indication` in slot N+1's tx_func** and use the
pipeline's `msgDataTx` directly. This ensures:
- Issue 1: No second `allocate_sched_response` → no leak (pipeline sched_response is
  deref'd at end of tx_func via `info->sched_response_id`)
- Issue 2: Single scheduling result used for both encoding and codeword → no mismatch
- Issue 3: Scheduler runs exactly once per slot → no double side effects

The trade-off: UL scheduling for slot N+1 happens one slot earlier (during slot N) instead of at
the start of slot N+1. This means HARQ feedback from slot N's RX is not yet available. However,
in the current code, MAC scheduling already runs BEFORE the RX chain is triggered
(`NR_slot_indication` at line 112 vs RX trigger at line 134 in `nr-gnb.c`), so the pipeline
only adds one more slot of staleness — acceptable per the existing MAC lookahead note.

#### Step 3.1: Define pipeline state

In `nr-gnb.c` or a new header, define the pipeline state:

```c
typedef enum {
  PIPELINE_IDLE,              // No pipeline data
  PIPELINE_ENCODING_INFLIGHT, // ACC100 processing + MAC scheduling done for next slot
  PIPELINE_SCHEDULED_ONLY     // MAC scheduling done but no PDSCH to encode (num_pdsch_slot==0)
} pipeline_state_t;

typedef struct {
  pipeline_state_t state;
  nrLDPC_encoding_context_t *enc_ctx;  // ACC100 context from submit (NULL if SCHEDULED_ONLY)
  int buf_idx;                          // which encoded output buffer has the results
  int target_frame;                     // frame the pipeline scheduling is for
  int target_slot;                      // slot the pipeline scheduling is for
  int target_msgDataTx_idx;             // which msgDataTx[] index was used for pipeline
  processingData_L1tx_t *target_msgTx;  // msgDataTx buffer used for pipeline scheduling
} encoding_pipeline_t;

static encoding_pipeline_t g_pipeline = {
  .state = PIPELINE_IDLE,
  .enc_ctx = NULL,
  .buf_idx = 0,
};
```

> **Why `PIPELINE_SCHEDULED_ONLY`?** When the pipeline runs `NR_slot_indication(N+1)` but
> `num_pdsch_slot == 0` (no DL data), we still need to skip `NR_slot_indication` in slot N+1
> to avoid Issue 3. This state tracks that scheduling was done but no encoding was submitted.

#### Step 3.2: Refactor nr_dlsch_encoding for submit-only mode

Create a new function `nr_dlsch_encoding_submit()` in `nr_dlsch_coding.c` that performs CRC + segmentation + ACC100 submit (but no collect):

```c
int nr_dlsch_encoding_submit(PHY_VARS_gNB *gNB,
                              processingData_L1tx_t *msgTx,
                              int frame, uint8_t slot,
                              NR_DL_FRAME_PARMS *frame_parms,
                              unsigned char *output,
                              persistent_encoding_params_t *persist,
                              nrLDPC_encoding_context_t **ctx_out)
{
  // Same CRC + segmentation as nr_dlsch_encoding() (lines 156-317)
  // But use persist->TBs[] and persist->segments[] instead of VLAs
  // ...

  // Submit to ACC100 (non-blocking)
  gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder_submit(
      &persist->slot_params, ctx_out);

  return 0;
}
```

Also create `nr_dlsch_encoding_collect()`:

```c
int nr_dlsch_encoding_collect(PHY_VARS_gNB *gNB,
                               nrLDPC_encoding_context_t *ctx,
                               persistent_encoding_params_t *persist)
{
  // Collect from ACC100 (blocks if not ready yet, usually ~0µs)
  int ret = gNB->nrLDPC_coding_interface.nrLDPC_coding_encoder_collect(ctx);

  // Merge per-segment stats (same as lines 341-348 in nr_dlsch_coding.c)
  // ...

  return ret;
}
```

#### Step 3.3: Create pipeline-aware encoding/codeword phases

In `nr_dlsch.c`, add new pipeline-aware functions:

```c
// Submit encoding for slot N+1 (called at end of slot N processing)
int nr_pdsch_encoding_submit(processingData_L1tx_t *msgTx, int frame, int slot,
                              int buf_idx, nrLDPC_encoding_context_t **ctx_out)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;
  // ... calculate output size (same as current encoding_phase) ...
  g_pdsch_encoded_size[buf_idx] = size_output;
  bzero(g_pdsch_encoded_output[buf_idx], size_output >> 3);

  return nr_dlsch_encoding_submit(gNB, msgTx, frame, slot,
                                   &gNB->frame_parms,
                                   g_pdsch_encoded_output[buf_idx],
                                   &g_enc_params[buf_idx],
                                   ctx_out);
}

// Collect encoding results (called at start of slot N processing for data submitted in slot N-1)
int nr_pdsch_encoding_collect(PHY_VARS_gNB *gNB, int buf_idx,
                               nrLDPC_encoding_context_t *ctx)
{
  return nr_dlsch_encoding_collect(gNB, ctx, &g_enc_params[buf_idx]);
}
```

#### Step 3.4: Implement the pipeline in tx_func

This is the core change. Restructure `tx_func()` in `nr-gnb.c`.

**Key design principle**: When pipeline data is available for the current slot, SKIP
`NR_slot_indication` and use the pipeline's `msgDataTx` directly. This prevents all three
critical issues (sched_response leak, scheduling mismatch, double scheduler invocation).

**sched_response lifecycle with pipeline**:
```
Slot N (pipeline priming):
  allocate_sched_response(A) → refcount=1
  nr_schedule_response: inc(A→2), deref(A→1)
  Pipeline: allocate_sched_response(B) → refcount=1
            nr_schedule_response: inc(B→2), deref(B→1)
            B stored in msgDataTx[next_buf]
  End: deref(A→0) → released ✓
  B stays alive (refcount=1) for slot N+1

Slot N+1 (pipeline hit):
  SKIP NR_slot_indication (B already in pipeline's msgDataTx)
  info = pipeline's msgDataTx → info->sched_response_id = B
  Pipeline: allocate_sched_response(C) → refcount=1
            C stored in msgDataTx[other_buf]
  End: deref(B→0) → released ✓
  C stays alive for slot N+2
```

```c
static void tx_func(processingData_L1tx_t *info)
{
  int frame_tx = info->frame;
  int slot_tx = info->slot;
  int frame_rx = info->frame_rx;
  int slot_rx = info->slot_rx;
  uint64_t original_timestamp_tx = info->timestamp_tx;
  PHY_VARS_gNB *gNB = info->gNB;
  module_id_t module_id = gNB->Mod_id;
  uint8_t CC_id = gNB->CC_id;
  NR_IF_Module_t *ifi = gNB->if_inst;
  nfapi_nr_config_request_scf_t *cfg = &gNB->gNB_config;

  SLOT_TIMING_START(frame_tx, slot_tx);
  int tx_slot_type = nr_slot_select(cfg, frame_tx, slot_tx);

  ru_tx_wait();

  if (slot_rx == 0) {
    reset_active_stats(gNB, frame_rx);
    reset_active_ulsch(gNB, frame_rx);
  }

  bool is_dl = (tx_slot_type == NR_DOWNLINK_SLOT || tx_slot_type == NR_MIXED_SLOT ||
                get_softmodem_params()->continuous_tx || IS_SOFTMODEM_RFSIM);

  // ====== STEP 1: MAC scheduling OR pipeline hit ======
  int enc_buf_idx = -1;
  int cur_buf;

  bool pipeline_hit = (g_pipeline.state != PIPELINE_IDLE &&
                        g_pipeline.target_frame == frame_tx &&
                        g_pipeline.target_slot == slot_tx);

  if (pipeline_hit) {
    // ---- Pipeline HIT: scheduling was already done in previous slot ----
    // DO NOT call NR_slot_indication — avoids Issues 1/2/3
    cur_buf = g_pipeline.target_msgDataTx_idx;
    info = g_pipeline.target_msgTx;
    info->timestamp_tx = original_timestamp_tx;
    info->gNB = gNB;

    if (g_pipeline.state == PIPELINE_ENCODING_INFLIGHT) {
      // Collect pre-encoded data from ACC100
      int ret = nr_pdsch_encoding_collect(gNB, g_pipeline.buf_idx, g_pipeline.enc_ctx);
      if (ret == 0) {
        enc_buf_idx = g_pipeline.buf_idx;
      } else {
        // ACC100 timeout or error — encoded data is invalid, fall back to synchronous
        LOG_E(PHY, "Pipeline collect FAILED for %d.%d (ret=%d), falling back to synchronous\n",
              frame_tx, slot_tx, ret);
        enc_buf_idx = -1;
      }
    }
    // else: PIPELINE_SCHEDULED_ONLY → no encoding to collect, enc_buf_idx stays -1

    g_pipeline.state = PIPELINE_IDLE;
    g_pipeline.enc_ctx = NULL;

    LOG_D(PHY, "Pipeline HIT for %d.%d (buf=%d, enc_buf=%d)\n",
          frame_tx, slot_tx, cur_buf, enc_buf_idx);
  } else {
    // ---- Pipeline MISS: run MAC scheduling normally ----
    // Handle stale pipeline data (frame/slot mismatch)
    if (g_pipeline.state == PIPELINE_ENCODING_INFLIGHT) {
      LOG_W(PHY, "Pipeline MISMATCH: expected %d.%d, got %d.%d. Collecting and discarding.\n",
            g_pipeline.target_frame, g_pipeline.target_slot, frame_tx, slot_tx);
      nr_pdsch_encoding_collect(gNB, g_pipeline.buf_idx, g_pipeline.enc_ctx);
      // Discard encoding results; also must release the stale pipeline's sched_response
      deref_sched_response(g_pipeline.target_msgTx->sched_response_id);
      g_pipeline.state = PIPELINE_IDLE;
      g_pipeline.enc_ctx = NULL;
    } else if (g_pipeline.state == PIPELINE_SCHEDULED_ONLY) {
      LOG_W(PHY, "Pipeline MISMATCH (sched-only): expected %d.%d, got %d.%d. Discarding.\n",
            g_pipeline.target_frame, g_pipeline.target_slot, frame_tx, slot_tx);
      deref_sched_response(g_pipeline.target_msgTx->sched_response_id);
      g_pipeline.state = PIPELINE_IDLE;
    }

    ifi->NR_slot_indication(module_id, CC_id, frame_tx, slot_tx);
    cur_buf = gNB->msgDataTx_idx;
    gNB->msgDataTx[cur_buf]->timestamp_tx = original_timestamp_tx;
    info = gNB->msgDataTx[cur_buf];
    info->gNB = gNB;
    enc_buf_idx = -1;  // Force synchronous encoding
  }

  // ====== STEP 2: Trigger RX chain (always, regardless of pipeline) ======
  LOG_D(NR_PHY, "Trigger RX for %d.%d\n", frame_rx, slot_rx);
  notifiedFIFO_elt_t *res = newNotifiedFIFO_elt(sizeof(processingData_L1_t), 0, &gNB->resp_L1, NULL);
  processingData_L1_t *syncMsg = NotifiedFifoData(res);
  syncMsg->gNB = gNB;
  syncMsg->frame_rx = frame_rx;
  syncMsg->slot_rx = slot_rx;
  syncMsg->timestamp_tx = info->timestamp_tx;
  res->key = slot_rx;
  pushNotifiedFIFO(&gNB->resp_L1, res);

  if (is_dl) {
    // ====== STEP 3: PHY Processing ======
    // enc_buf_idx >= 0: skip encoding, use pre-encoded data from buf_idx
    // enc_buf_idx < 0:  run synchronous encoding (fallback)
    phy_procedures_gNB_TX(info, frame_tx, slot_tx, 1, enc_buf_idx);

    // ====== STEP 4: Submit encoding for NEXT slot (pipeline lookahead) ======
    // Determine next slot type
    int next_frame = frame_tx;
    int next_slot = slot_tx + 1;
    if (next_slot >= gNB->frame_parms.slots_per_frame) {
      next_slot = 0;
      next_frame = (frame_tx + 1) % 1024;
    }
    int next_slot_type = nr_slot_select(cfg, next_frame, next_slot);
    bool next_is_dl = (next_slot_type == NR_DOWNLINK_SLOT || next_slot_type == NR_MIXED_SLOT);

    if (next_is_dl) {
      // Switch to the other msgDataTx buffer for lookahead scheduling
      int next_buf = 1 - cur_buf;
      gNB->msgDataTx_idx = next_buf;

      // MAC scheduling for next slot (runs scheduler exactly once for slot N+1)
      ifi->NR_slot_indication(module_id, CC_id, next_frame, next_slot);
      processingData_L1tx_t *next_msgTx = gNB->msgDataTx[next_buf];

      // Store pipeline state
      g_pipeline.target_frame = next_frame;
      g_pipeline.target_slot = next_slot;
      g_pipeline.target_msgDataTx_idx = next_buf;
      g_pipeline.target_msgTx = next_msgTx;

      if (next_msgTx->num_pdsch_slot > 0) {
        // Submit encoding to ACC100 (non-blocking)
        int next_enc_buf = 1 - (enc_buf_idx >= 0 ? enc_buf_idx : 0);
        nrLDPC_encoding_context_t *ctx = NULL;
        nr_pdsch_encoding_submit(next_msgTx, next_frame, next_slot,
                                  next_enc_buf, &ctx);
        g_pipeline.state = PIPELINE_ENCODING_INFLIGHT;
        g_pipeline.enc_ctx = ctx;
        g_pipeline.buf_idx = next_enc_buf;
      } else {
        // No PDSCH but scheduling is done — mark so we skip NR_slot_indication in next slot
        g_pipeline.state = PIPELINE_SCHEDULED_ONLY;
        g_pipeline.enc_ctx = NULL;
      }

      // NOTE: Do NOT deref next_msgTx->sched_response_id here!
      // It must stay alive until next slot's tx_func deref's it via info->sched_response_id.

      // Restore current buffer index (so non-pipeline code paths see correct value)
      gNB->msgDataTx_idx = cur_buf;
    }

    // ====== STEP 5: RU Processing (async) ======
    processingData_RU_t syncMsgRU;
    syncMsgRU.frame_tx = frame_tx;
    syncMsgRU.slot_tx = slot_tx;
    syncMsgRU.ru = gNB->RU_list[0];
    syncMsgRU.timestamp_tx = info->timestamp_tx;
    ru_tx_func_async(&syncMsgRU);
  }

  SLOT_TIMING_END();

  // Release current slot's sched_response
  // For pipeline hit: this releases the pipeline sched_response (allocated in previous slot)
  // For normal: this releases the sched_response allocated at the start of this tx_func
  if (NFAPI_MODE == NFAPI_MONOLITHIC) {
    deref_sched_response(info->sched_response_id);
  }
}
```

> **IMPORTANT NOTE ON MAC LOOKAHEAD**: The MAC scheduler's `NR_slot_indication()` for slot N+1 runs during slot N's tx_func. This means:
> - DL scheduling decisions for N+1 are made one slot early
> - CSI/CQI feedback from slot N is not yet processed (RX chain for slot N hasn't completed)
> - HARQ feedback from slot N is not yet processed
> - This is acceptable because:
>   (a) CSI feedback has a multi-slot processing delay anyway
>   (b) HARQ feedback takes k1 ≥ 6 slots to arrive — pipeline offset is irrelevant (see Phase 5 Step 5.1)
>   (c) In the current code, MAC scheduling already runs BEFORE RX chain trigger
>       (`NR_slot_indication` at `nr-gnb.c:112` vs RX trigger at line 134),
>       so the pipeline only adds one more slot of staleness
>   (d) The `gNB->sched_lock` mutex (`gNB_scheduler.c:172`) ensures no concurrent
>       access even though scheduling runs at an unusual time
>
> **sched_response pool capacity**: With `N_RESP=3`, at most 2 are in use simultaneously
> (current slot + pipeline's next slot). Pool capacity is sufficient.
>
> **Buffer index alternation**: Pipeline uses `msgDataTx[0]` and `msgDataTx[1]` alternately:
> ```
> Slot N:   current=buf[0], pipeline=buf[1]
> Slot N+1: current=buf[1] (pipeline hit), pipeline=buf[0]
> Slot N+2: current=buf[0] (pipeline hit), pipeline=buf[1]
> ```

#### Step 3.5: Modify phy_procedures_gNB_TX() for pipeline

In `phy_procedures_nr_gNB.c`, add the `enc_buf_idx` parameter:

```c
void phy_procedures_gNB_TX(processingData_L1tx_t *msgTx,
                           int frame, int slot, int do_meas,
                           int pipeline_enc_buf_idx)  // NEW: -1 = synchronous, >=0 = use pre-encoded
{
  // ... existing setup ...

  // 1. Start async memory clear
  enkits_pool_memclear_tx_async(...);

  // 2. Encoding (conditional on pipeline state)
  if (pipeline_enc_buf_idx < 0) {
    // FALLBACK: No pipeline data available, run synchronous encoding
    int buf_idx = 0;  // Default buffer; pipeline submit uses 1 - buf_idx to avoid conflict
    if (pdsch_count > 0) {
      nr_pdsch_encoding_phase(msgTx, frame, slot, buf_idx);
    }
    pipeline_enc_buf_idx = buf_idx;
  }
  // else: encoding was already done in previous slot's submit, data is in pipeline_enc_buf_idx

  // 3. Wait for memclear
  enkits_pool_memclear_tx_wait();

  // 4. PRS/SSB/PDCCH (unchanged)
  // ...

  // 5. Codeword phase with pipeline buffer
  if (pdsch_count > 0) {
    nr_pdsch_codeword_phase(msgTx, frame, slot, pipeline_enc_buf_idx);
  }

  // 6. CSI-RS + phase rotation (unchanged)
  // ...
}
```

#### Verification (Phase 3)

1. **Build test**: Compile cleanly with standard build command (including dlsim/ulsim)
2. **sched_response leak test**: Run for 30+ seconds with logging:
   ```c
   // Temporary debug: add to deref_sched_response() in nr_sched_response.c
   LOG_I(NR_MAC, "deref sched_response %d: refcount %d→%d\n",
         sched_response_id, resp_refcount[sched_response_id],
         resp_refcount[sched_response_id] - 1);
   ```
   Verify: every `allocate` has a matching `deref→0`. No `exit(1)` at line 89.
3. **Pipeline hit/miss logging**: Verify LOG output shows expected pattern for DDDSU:
   ```
   Slot 0 (D): Pipeline MISS (first DL) → synchronous encoding
   Slot 1 (D): Pipeline HIT → collect (~0µs)
   Slot 2 (D): Pipeline HIT → collect (~0µs)
   Slot 3 (S): Pipeline HIT → collect (~0µs), NO pipeline submit (next is U)
   Slot 4 (U): Pipeline IDLE
   Slot 5 (D): Pipeline MISS (first DL after UL) → synchronous encoding
   ```
4. **Scheduling consistency check**: Add temporary validation in `phy_procedures_gNB_TX`:
   ```c
   if (pipeline_enc_buf_idx >= 0) {
     // Pipeline data should match msgTx parameters
     LOG_I(PHY, "Pipeline: num_pdsch=%d, enc_buf=%d\n",
           msgTx->num_pdsch_slot, pipeline_enc_buf_idx);
   }
   ```
5. **Timing test**: Check `l1_slot_timing.csv`:
   - Slot 0 (first DL): encoding ~193µs (synchronous, pipeline priming)
   - Slots 1-2 (subsequent DL): encoding ~5µs (collect only)
   - Slot 3 (last DL before S): encoding ~5µs (collect, no submit for next)
6. **Pipeline mismatch test**: Verify mismatch handling works:
   - Kill UE mid-slot to trigger scheduling change
   - Verify LOG shows "Pipeline MISMATCH" and graceful fallback
   - Verify sched_response is properly released in mismatch path
7. **Error handling test**: Kill/restart gNB to verify pipeline state resets cleanly

---

### Phase 4: TDD Pattern Awareness and Edge Cases

**Goal**: Verify and document that TDD pattern transitions, FDD mode, UL slots, special slots,
and error recovery are all correctly handled by Phase 3's pipeline implementation.

> **Note**: Most edge cases are already handled by Step 3.4's pipeline state machine.
> This phase documents the analysis and adds any remaining edge case handling.

**Files to modify**:
- `executables/nr-gnb.c` (minor additions only)

#### Step 4.1: TDD pattern analysis (DDDSU)

The deployment config (`gnb.sa.band78.273prb.fhi72.4x4-liteon.conf`) defines:
- `dl_UL_TransmissionPeriodicity = 5` (2.5ms period)
- `nrofDownlinkSlots = 3`, `nrofDownlinkSymbols = 6`
- `nrofUplinkSlots = 1`, `nrofUplinkSymbols = 4`
- SCS 30kHz → 20 slots/frame → 4 periods → pattern: **DDDSU DDDSU DDDSU DDDSU**

Pipeline behavior per 5-slot period (verified against Step 3.4 code):

```
Slot:     0(D)     1(D)     2(D)     3(S)     4(U)
          ──────── ──────── ──────── ──────── ────────
Step 1:   miss     hit      hit      hit      miss
          NR_slot  skip     skip     skip     NR_slot
          _ind(0)  (use     (use     (use     _ind(4)
                   pipe)    pipe)    pipe)
Encoding: sync     collect  collect  collect  (none)
Step 4:   submit   submit   submit   (no sub  (no sub
          for 1    for 2    for 3    next=U)  is_dl=F)
          ──────── ──────── ──────── ──────── ────────
Result:   sync     pipe     pipe     drain    idle
```

Key transitions:
- **D→D (slots 0→1, 1→2)**: `next_is_dl=true` → pipeline submit ✓
- **D→S (slot 2→3)**: `next_slot_type=NR_MIXED_SLOT` → `next_is_dl=true` → pipeline submit ✓
  (Mixed slots CAN have PDSCH in their 6 DL symbols)
- **S→U (slot 3→4)**: `next_slot_type=NR_UPLINK_SLOT` → `next_is_dl=false` → no submit (drain) ✓
- **U→D (slot 4→5)**: `is_dl=false` for UL → no pipeline step → IDLE → slot 5 does sync ✓

> **How `nr_slot_select()` works** (`phy_frame_config_nr.c:155-189`):
> The function indexes `cfg->tdd_table.max_tdd_periodicity_list[nr_slot]` which is allocated
> for ALL `numb_slots_frame` slots (line 74). The TDD pattern is replicated across all periods.
> The frame number is ignored (`(void) nr_frame` at line 157). This means `next_slot_type`
> is always correct regardless of frame boundaries.

#### Step 4.2: FDD mode compatibility

For FDD (`frame_duplex_type == FDD`), `nr_slot_select()` returns
`NR_UPLINK_SLOT | NR_DOWNLINK_SLOT = 0x02 | 0x01 = 0x03 = NR_MIXED_SLOT` (line 163).

Pipeline behavior in FDD:
- `is_dl`: `tx_slot_type == NR_MIXED_SLOT` → **true** for every slot ✓
- `next_is_dl`: `next_slot_type == NR_MIXED_SLOT` → **true** for every slot ✓
- Result: **continuous pipeline** — every slot submits for the next. Optimal performance.

No special handling needed for FDD. The existing `NR_MIXED_SLOT` checks cover it naturally.

#### Step 4.3: `continuous_tx` and RFSIM mode

The `is_dl` check in current `tx_func()` (`nr-gnb.c:136`) includes:
```c
get_softmodem_params()->continuous_tx || IS_SOFTMODEM_RFSIM
```

But the pipeline's `next_is_dl` check (Step 3.4) does NOT include these flags:
```c
bool next_is_dl = (next_slot_type == NR_DOWNLINK_SLOT || next_slot_type == NR_MIXED_SLOT);
```

This creates an asymmetry for TDD UL slots with `continuous_tx`/RFSIM:
- `is_dl = true` (enters DL processing block) but `next_is_dl` may be false
- PHY processing runs with `pdsch_count=0` (MAC doesn't schedule PDSCH for UL symbols)
- Pipeline submit for next slot depends only on the **next** slot's type, not `continuous_tx`

**This is actually beneficial**: In DDDSU with continuous_tx, slot 4 (UL) has `is_dl=true`,
enters the DL block, and `next_is_dl=true` for slot 5 (D). So slot 4 **primes the pipeline
for slot 5**, eliminating the sync penalty at the first DL after UL:
```
continuous_tx DDDSU:
  Slot:     0(D)    1(D)    2(D)    3(S)    4(U)    5(D)
  Pipeline: sync    pipe    pipe    drain   prime   pipe ← improved!
```

Without `continuous_tx`, slot 4's `is_dl=false` skips the pipeline block, so slot 5 must sync.
This is a free optimization for RFSIM testing. No code changes needed.

#### Step 4.4: Pipeline invalidation scenarios

All invalidation scenarios are handled by Step 3.4's pipeline miss path:

| Scenario | Detection | Handling |
|---|---|---|
| No PDSCH in next slot | `num_pdsch_slot == 0` | `PIPELINE_SCHEDULED_ONLY` state |
| ACC100 timeout | `collect()` returns non-zero | `enc_buf_idx = -1` → synchronous fallback |
| Frame/slot mismatch | `target_frame/slot != frame_tx/slot_tx` | Collect+discard + deref stale sched_response |
| Slot skipped (timing overflow) | Same as frame/slot mismatch | Same handling |
| HARQ retransmission | See Phase 5 Step 5.1 | Safe to pipeline — same encoding path, k1 ≥ 6 makes staleness irrelevant |

No additional safety checks needed.

#### Step 4.5: Frame/slot wraparound

Already implemented in Step 3.4:
```c
int next_slot = slot_tx + 1;
if (next_slot >= gNB->frame_parms.slots_per_frame) {
  next_slot = 0;
  next_frame = (frame_tx + 1) % 1024;
}
```

Since `nr_slot_select()` ignores the frame number (`phy_frame_config_nr.c:157`) and the
TDD table covers all `slots_per_frame` entries, the wraparound is transparent. The pipeline
match check uses both `target_frame` and `target_slot`, correctly distinguishing frame 1023
slot 19 from frame 0 slot 0.

#### Step 4.6: First-slot-after-boot and gNB restart

At gNB startup, `g_pipeline.state = PIPELINE_IDLE` (static initializer), so the first slot
always takes the synchronous path. No special handling needed.

On gNB shutdown/restart, `g_pipeline` is static and re-initialized. If the pipeline holds
ACC100 resources (ENCODING_INFLIGHT), these must be collected before shutdown:

```c
// In term_gNB_Tpool() or shutdown path:
if (g_pipeline.state == PIPELINE_ENCODING_INFLIGHT) {
  nr_pdsch_encoding_collect(gNB, g_pipeline.buf_idx, g_pipeline.enc_ctx);
  deref_sched_response(g_pipeline.target_msgTx->sched_response_id);
  g_pipeline.state = PIPELINE_IDLE;
} else if (g_pipeline.state == PIPELINE_SCHEDULED_ONLY) {
  deref_sched_response(g_pipeline.target_msgTx->sched_response_id);
  g_pipeline.state = PIPELINE_IDLE;
}
```

#### Verification (Phase 4)

1. **TDD pattern test**: Run with DDDSU pattern, verify LOG output:
   - Slots 0,5,10,15: Pipeline MISS (sync)
   - Slots 1,2,6,7,11,12,16,17: Pipeline HIT (pipe)
   - Slots 3,8,13,18: Pipeline HIT + no submit (drain)
   - Slots 4,9,14,19: Pipeline IDLE (UL)
2. **No-PDSCH test**: Verify that when MAC doesn't schedule PDSCH, pipeline enters
   `PIPELINE_SCHEDULED_ONLY` state and slot N+1 still skips `NR_slot_indication`
3. **Frame wraparound test**: Run for >1 frame (10ms), verify no pipeline errors at slot 19→0
4. **Long-run test**: Run for 5 minutes, verify no memory leaks or ACC100 resource exhaustion
5. **RFSIM test**: Run with `--rfsim`, verify pipeline primes from UL slots (continuous_tx)
6. **ACC100 error test**: Inject timeout (increase `TIME_OUT_POLL` threshold temporarily),
   verify collect failure → synchronous fallback → normal operation continues
7. **Shutdown test**: Send SIGTERM during pipeline-active slot, verify no ACC100 resource leak

---

### Phase 5: MAC Scheduler Lookahead Considerations

**Goal**: Ensure MAC scheduling for slot N+1 during slot N is safe and produces correct results.

**Critical Analysis (verified against code)**:

The pipeline calls `NR_slot_indication(N+1)` at the end of slot N's `tx_func`, which is ~33µs
earlier than the normal call at the start of slot N+1's `tx_func`. The MAC scheduler makes
scheduling decisions based on state that may be slightly older:

| Input | Staleness Impact | Code Reference |
|---|---|---|
| Buffer status reports (BSR) | Stale by ~33µs — negligible, BSR already stale by UL processing delay | `gNB_scheduler.c` |
| CQI/PMI feedback | Stale by ~33µs — negligible, CQI already has ~4 slot processing delay | `gNB_scheduler_uci.c` |
| HARQ ACK/NACK | **Not affected** — see Step 5.1 analysis | `gNB_scheduler_uci.c:396-415` |
| DRX timers | Stale by ~33µs — negligible impact | `nr_mac_update_timers()` |
| SRS measurements | Stale by ~33µs — already stale by processing delay | `gNB_scheduler.c` |

#### Step 5.1: HARQ retransmission analysis

> **Code-verified finding**: HARQ feedback timing makes the 1-slot pipeline offset irrelevant
> for retransmission concerns.

**HARQ DL feedback timing (verified in code)**:

The HARQ-ACK/NACK for a PDSCH in slot N is sent by the UE on PUCCH in slot N+k1, where k1
is configured via `dl_DataToUL_ACK` (`nr_radio_config.c:1163-1173`):

```
k1[i] = i + min_feedback_time    (i = 0..7)
```

For the DDDSU TDD pattern (30kHz SCS, `gnb.sa.band78.273prb.fhi72.4x4-liteon.conf`):
- `min_feedback_time` is typically 6 (minRXTXTIME)
- k1 values: [6, 7, 8, 9, 10, 11, 12, 13] slots
- After PUCCH reception and L1/L2 processing, the NACK is available to the MAC scheduler
  several MORE slots later

**Consequence**: When the pipeline schedules slot N+1 during slot N, the HARQ feedback
for slot N hasn't even been SENT by the UE yet (k1 ≥ 6 slots away). The 1-slot (~33µs)
pipeline offset has zero impact on HARQ feedback availability.

**Retransmissions in the pipeline**: If the scheduler for N+1 schedules a retransmission
(of some earlier slot M where a NACK has already been received and processed), this is
**safe to pipeline**:

1. The retransmission decision was already made by the scheduler (`gNB_scheduler_dlsch.c:1072-1075`:
   `harq->round > 0` → `retrans_dl_harq` list)
2. The RV index is computed from the HARQ round (`nr_get_rv(round % 4)` → `{0, 2, 3, 1}`)
   and stored in `pdsch_pdu.pdsch_pdu_rel15.rvIndex[0]` (`gNB_scheduler_dlsch.c:956`)
3. The TB data is from the HARQ buffer (`harq->pdu` — same data as original transmission)
4. The encoding path (`nr_dlsch_encoding` → `nr_dlsch_coding.c:285`) extracts `rel15->rvIndex[0]`
   and passes it to the LDPC encoder, which handles any RV value identically
5. The ACC100 hardware processes all RV values through the same code path

**Conclusion**: No special retransmission handling is needed. Step 3.4's code correctly
pipelines all PDSCH (both new transmissions and retransmissions) when `num_pdsch_slot > 0`.
The `rvIndex` check is NOT required.

> **Previous draft had an incorrect `rvIndex[0] != 0` check** to exclude retransmissions
> from the pipeline. This has been removed because:
> - The timing scenario it addressed (NACK arriving during pipeline) is impossible (k1 ≥ 6)
> - Retransmission encoding is identical to new transmission encoding (same code path)
> - The check was also inconsistent with Step 3.4 which has no such filter

#### Step 5.2: Scheduler side effects and re-entrancy

> **CRITICAL**: `gNB_dlsch_ulsch_scheduler()` (`gNB_scheduler.c:163-271`) is **NOT stateless**.
> It has significant side effects including:
> - `nr_mac_update_timers()` (line 205): advances HARQ/BSR/PHR timers
> - VRB map clearing (lines 184-194): resets resource allocation maps
> - `nr_schedule_RA()` (line 244): RA procedure state machine
> - `nr_schedule_ulsch()` (line 249): UL grant allocation
> - `nr_schedule_pucch()` (line 259): PUCCH scheduling
>
> **It MUST NOT be called twice for the same slot.** Phase 3's redesigned Step 3.4 ensures
> this by skipping `NR_slot_indication` when pipeline data is available (pipeline hit path).
> The scheduler runs exactly once per slot: either during the pipeline step of the previous
> slot, or during the normal flow of the current slot.

The MAC scheduler is called for slot N+1 during slot N's pipeline step. In OAI's monolithic
mode, `NR_slot_indication()` → `gNB_dlsch_ulsch_scheduler()` runs synchronously in the L1 TX
thread. This is safe because:
- Only one `gNB_dlsch_ulsch_scheduler()` call is active at any time (single-threaded `tx_func`)
- The `sched_lock` mutex is held during the entire scheduler call (`gNB_scheduler.c:172/269`)
- The `sched_response` pool has N_RESP=3 entries, supporting 2 live responses (current + pipeline)
- The N+1 scheduler call happens AFTER the N scheduler completes (sequential in `tx_func`)

#### Step 5.3: `num_pdsch_slot` lifecycle in pipeline context

The `num_pdsch_slot` field in `processingData_L1tx_t` controls how many PDSCH PDUs are
processed. Its lifecycle in the pipeline must be understood:

**Population** (`nr_dlsch_tools.c:37-49`):
```
nr_schedule_dl_tti_req() → nr_fill_dlsch_dl_tti_req():
  - Copies PDSCH PDU into dlsch[num_pdsch_slot]->harq_process.pdsch_pdu
  - Asserts pduIndex == num_pdsch_slot (ordering guarantee)
  - Increments num_pdsch_slot++
```

**Reset** (`phy_procedures_nr_gNB.c:555`):
```
After apply_nr_rotation_TX(): msgTx->num_pdsch_slot = 0
```

**Pipeline flow**:
```
Slot N:
  1. Scheduler for N+1 → populates msgDataTx[next_buf]->num_pdsch_slot via NFAPI
  2. Pipeline reads num_pdsch_slot to decide ENCODING_INFLIGHT vs SCHEDULED_ONLY
  3. Current slot N finishes → resets msgDataTx[cur_buf]->num_pdsch_slot = 0

Slot N+1 (pipeline hit):
  4. Uses msgDataTx[next_buf] which still has num_pdsch_slot from step 1
  5. Encoding and codeword phases loop over dlsch[0..num_pdsch_slot-1]
  6. After processing → resets msgDataTx[next_buf]->num_pdsch_slot = 0
```

The dual-buffer design (`msgDataTx[0]` / `msgDataTx[1]`) ensures no conflict: slot N's
reset (step 3) operates on `cur_buf`, while the pipeline data lives in `next_buf`.

#### Verification (Phase 5)

1. **HARQ timing test**: Verify that HARQ NACKs received during slot N do NOT affect the
   pipeline's encoding for slot N+1 (NACKs are for slots ≤ N-k1, already incorporated by scheduler)
2. **Retransmission pipeline test**: Force a retransmission scenario, verify pipeline handles
   `rvIndex > 0` correctly (same encoding quality as synchronous path)
3. **Scheduler once-per-slot test**: Add assertion in `gNB_dlsch_ulsch_scheduler()` that
   `(frame, slot)` never repeats without advancing. Verify under pipeline operation.
4. **sched_response test**: Monitor `resp_refcount[]` via logging, verify no exhaustion
   (N_RESP=3 supports 2 concurrent: current + pipeline)
5. **num_pdsch_slot integrity test**: Log `num_pdsch_slot` at pipeline submission and
   collection points, verify values match
6. **Late HARQ ACK edge case**: If a HARQ ACK arrives between the pipeline's scheduler
   call (during slot N) and normal slot N+1 processing, the retransmission still proceeds.
   This is harmless — the UE receives a redundant transmission and ACKs it.

---

### Phase 6: Performance Measurement and Tuning

**Goal**: Measure pipeline performance and fine-tune.

#### Step 6.1: Add pipeline-specific timing fields

**Location**: `openair1/SCHED_NR/nr_slot_timing.h` (existing `slot_timing_t` struct, lines 42-98)

The existing timing infrastructure already captures 34 CSV columns including `encoding_overlap_ns`,
`memclear_wait_ns`, `pdsch_encoding_ns`, etc. On pipeline-hit slots, existing fields automatically
reflect the benefit (e.g., `encoding_overlap_ns` drops to ~0µs). Add supplementary fields for
pipeline-specific diagnostics:

```c
// Add to slot_timing_t struct in nr_slot_timing.h:

// Pipeline timing (Phase 6)
long pipeline_collect_ns;    // Time to collect from ACC100 (in tx_func, before phy_proc)
long pipeline_submit_ns;     // Time to submit encoding to ACC100 (end of tx_func)
long mac_lookahead_ns;       // Time for NR_slot_indication(N+1) (end of tx_func)
int  pipeline_state;         // 0=synchronous, 1=pipeline_hit, 2=pipeline_primed
```

**Cross-slot measurement note**: Pipeline measurements span two slots:
- **Slot N (pipeline priming)**: `mac_lookahead_ns` and `pipeline_submit_ns` are recorded.
  `pipeline_state = 2` (primed).
- **Slot N+1 (pipeline hit)**: `pipeline_collect_ns` is recorded, `encoding_overlap_ns` ≈ 0.
  `pipeline_state = 1` (hit).
- To compute the full pipeline benefit, compare two consecutive CSV rows.

**Instrumentation points in Step 3.4** (`nr-gnb.c` tx_func):
```c
// In pipeline-hit path:
if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_collect_start);
int ret = nr_pdsch_encoding_collect(gNB, g_pipeline.buf_idx, g_pipeline.enc_ctx);
if (slot_timing_enabled) {
  clock_gettime(CLOCK_MONOTONIC, &t_collect_end);
  current_slot_timing.pipeline_collect_ns = timespec_diff_ns_timing(&t_collect_start, &t_collect_end);
  current_slot_timing.pipeline_state = 1;  // pipeline hit
}

// In pipeline-priming path (STEP 4):
if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_lookahead_start);
ifi->NR_slot_indication(module_id, CC_id, next_frame, next_slot);
if (slot_timing_enabled) {
  clock_gettime(CLOCK_MONOTONIC, &t_lookahead_end);
  current_slot_timing.mac_lookahead_ns = timespec_diff_ns_timing(&t_lookahead_start, &t_lookahead_end);
}

if (slot_timing_enabled) clock_gettime(CLOCK_MONOTONIC, &t_submit_start);
nr_pdsch_encoding_submit(next_msgTx, next_frame, next_slot, next_enc_buf, &ctx);
if (slot_timing_enabled) {
  clock_gettime(CLOCK_MONOTONIC, &t_submit_end);
  current_slot_timing.pipeline_submit_ns = timespec_diff_ns_timing(&t_submit_start, &t_submit_end);
  current_slot_timing.pipeline_state = 2;  // pipeline primed
}
```

**CSV columns to add** (extend `slot_timing_end_and_log()` in `nr_slot_timing.c`):
```
...,pipeline_collect_ns,pipeline_submit_ns,mac_lookahead_ns,pipeline_state
```

#### Step 6.2: Expected timing results

**Verified against code**: The current flow in `phy_procedures_nr_gNB.c:311-346`:
1. `enkits_pool_memclear_tx_async()` — starts background memclear (~31µs)
2. `nr_pdsch_encoding_phase()` — encoding runs in parallel (~160µs >> 31µs)
3. `enkits_pool_memclear_tx_wait()` — wait ≈ 0µs (encoding hides memclear)

With pipeline (enc_buf_idx ≥ 0), encoding is **skipped** inside `phy_procedures_gNB_TX`,
so memclear has no parallel work. **Memclear wait increases from ~0µs to ~31µs**.

> **Design note**: Collection happens in `tx_func` BEFORE `phy_procedures_gNB_TX`,
> so the encoding slot (step 2) is empty. A future optimization could move collection
> inside `phy_procedures_gNB_TX` to run in the encoding slot (~5µs parallel with ~31µs
> memclear), reducing memclear wait to ~26µs.

**2-layer 273PRB timing comparison** (estimated):

| Phase | Synchronous (no pipeline) | Pipeline-HIT slot | Notes |
|---|---|---|---|
| MAC scheduling | ~100µs | **~0µs (skipped)** | Pipeline hit skips NR_slot_indication |
| Collection from ACC100 | N/A | ~5µs | In tx_func, before phy_proc |
| Encoding (CRC+Seg+ACC100) | ~160µs | **~0µs (skipped)** | Already collected from pipeline |
| Memclear wait | ~0µs | **~31µs** | Encoding no longer hides memclear |
| DMRS precompute wait | ~0µs | ~0µs | Hidden by memclear (~31µs > DMRS ~5µs) |
| PRS/SSB/PDCCH | ~15µs | ~15µs | |
| Codeword phase | ~70µs | ~70µs | |
| Phase rotation | ~10µs | ~10µs | |
| MAC lookahead (N+1) | N/A | ~100µs | NR_slot_indication for next slot |
| Pipeline submit | N/A | ~5µs | Non-blocking ACC100 enqueue |
| **total_slot_ns** | **~355µs** | **~236µs** | **~119µs saving (34%)** |

**Breakdown of savings**:
- Removed: encoding ~160µs (replaced by ~5µs collection + ~31µs memclear wait = ~36µs → net −124µs)
- Removed: current-slot MAC scheduling ~100µs (skipped on pipeline hit → −100µs)
- Added: MAC lookahead ~100µs + pipeline submit ~5µs (→ +105µs)
- **Net saving: −124µs − 100µs + 105µs = −119µs**

> **Key insight**: MAC scheduling for each DL slot runs exactly once. The pipeline shifts
> WHEN it runs (from start of slot N+1 to end of slot N). The per-slot system-wide work
> is unchanged — only the distribution across slots changes.

**4-layer 273PRB timing comparison** (estimated):

| Phase | Synchronous | Pipeline-HIT slot | Notes |
|---|---|---|---|
| MAC scheduling | ~100µs | ~0µs (skipped) | |
| Collection | N/A | ~5µs | |
| Encoding (4-layer) | ~250µs (est.) | ~0µs (skipped) | 2× segments vs 2-layer |
| Memclear wait | ~0µs | ~31µs | Same txdataF size |
| PRS/SSB/PDCCH | ~15µs | ~15µs | |
| Codeword phase (4-layer) | ~140µs (est.) | ~140µs | 2× layers, precoding needed |
| Phase rotation | ~10µs | ~10µs | |
| MAC lookahead + Submit | N/A | ~105µs | |
| **total_slot_ns** | **~515µs** | **~306µs** | **~209µs saving (41%)** |

> **Warning**: 4-layer without pipeline (~515µs) approaches the 500µs slot budget (30kHz SCS).
> The pipeline is critical for 4-layer operation.

**First slot after boot / UL gap (pipeline miss)**:
On the first DL slot (no pipeline data available), processing is synchronous:
- Full MAC scheduling + encoding + signals + pipeline priming for next slot
- `total_slot_ns` ≈ 355µs + 105µs (MAC lookahead) = ~460µs (2-layer)
- This only happens once per DL burst; subsequent slots benefit from pipeline

**Existing CSV fields behavior under pipeline** (for reference):

| CSV Column | Synchronous | Pipeline-HIT | Pipeline-priming |
|---|---|---|---|
| `mac_scheduler_ns` | ~100µs | ~0µs (skipped) | ~100µs (current slot) |
| `encoding_overlap_ns` | ~160µs | ~0µs (skipped) | ~160µs |
| `memclear_wait_ns` | ~0µs | ~31µs | ~0µs |
| `pdsch_encoding_ns` | ~160µs | ~0µs | ~160µs |
| `enc_crc_ns` | ~3µs | 0 | ~3µs |
| `enc_ldpc_ns` | ~150µs | 0 | ~150µs |

#### Verification (Phase 6)

1. **Timing CSV collection**: Run with pipeline enabled and `--enable-l1-timing`:
   ```bash
   sudo ./nr-softmodem -O ...conf --enable-encoding-pipeline [other flags]
   ```
   Collect `l1_slot_timing.csv` for at least 1000 DL slots (100+ frames).

2. **Pipeline-hit identification**: Filter CSV rows by `pipeline_state`:
   ```bash
   # pipeline_state: 0=synchronous, 1=hit, 2=primed
   awk -F',' '$38==1' l1_slot_timing.csv > pipeline_hit_slots.csv
   awk -F',' '$38==0' l1_slot_timing.csv > synchronous_slots.csv
   ```

3. **Key metrics to validate**:
   - `encoding_overlap_ns` ≈ 0 on pipeline-hit rows (encoding was skipped)
   - `memclear_wait_ns` ≈ 31µs on pipeline-hit rows (memclear not hidden)
   - `mac_scheduler_ns` ≈ 0 on pipeline-hit rows (NR_slot_indication skipped)
   - `pipeline_collect_ns` ≈ 5µs on pipeline-hit rows
   - `mac_lookahead_ns` ≈ 100µs on pipeline-priming rows
   - `total_slot_ns` reduction of ~100-200µs on pipeline-hit vs synchronous rows

4. **A/B comparison**: Use `analyze_l1_slot_timing.py` to compare pipeline-enabled vs disabled:
   ```bash
   python3 analyze_l1_slot_timing.py --compare pipeline_enabled.csv pipeline_disabled.csv
   ```

5. **Jitter analysis**: Compute stddev of `total_slot_ns` for pipeline-hit slots vs synchronous.
   Pipeline should reduce jitter (encoding time variance removed from critical path).

6. **4-layer validation**: Run with `maxMIMO_layers=4`, verify `total_slot_ns < 500µs`
   for pipeline-hit slots. This is the critical use case — 4-layer may not fit in 500µs budget
   without the pipeline.

7. **First-slot overhead**: Verify the first DL slot in each DDDSU burst (synchronous + pipeline
   priming) completes within budget. This is the worst-case slot.

---

## Appendix A: Complete File Change Summary

| File | Changes | Phase |
|---|---|---|
| `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h` | Add `nrLDPC_encoding_context_t`, submit/collect typedefs, extend interface struct | 1 |
| `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface_load.c` | Optional dlsym for submit/collect symbols | 1 |
| `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_aal/nrLDPC_coding_aal.c` | Implement context struct, submit, collect (exported symbols) | 1 |
| `openair1/PHY/NR_TRANSPORT/nr_dlsch.c` | Double-buffer `g_pdsch_encoded_output`, add `buf_idx` params to encoding/codeword phases | 1 |
| `openair1/PHY/NR_TRANSPORT/nr_dlsch_coding.c` | Persistent encoding params, `nr_dlsch_encoding_submit()`, `nr_dlsch_encoding_collect()` | 1, 3 |
| `openair1/PHY/NR_TRANSPORT/nr_dlsch.h` | Export new function prototypes | 1, 3 |
| `openair1/PHY/defs_gNB.h` | `msgDataTx` → `msgDataTx[2]` + `msgDataTx_idx` | 2 |
| `executables/nr-gnb.c` | Dual msgDataTx alloc, pipeline state machine in tx_func | 2, 3 |
| `openair1/SCHED_NR/fapi_nr_l1.c` | Route to `msgDataTx[msgDataTx_idx]` | 2 |
| `openair1/SCHED_NR/phy_procedures_nr_gNB.c` | Add `pipeline_enc_buf_idx` param, conditional encoding skip | 3 |
| `openair1/SIMULATION/NR_PHY/dlsim.c` | Update `msgDataTx` → `msgDataTx[0]` | 2 |
| `openair1/SIMULATION/NR_PHY/ulsim.c` | Update `msgDataTx` → `msgDataTx[0]` | 2 |
| `openair1/SCHED_NR/nr_slot_timing.h` | Add pipeline timing fields + CSV columns | 6 |
| `openair1/SCHED_NR/nr_slot_timing.c` | Extend `slot_timing_end_and_log()` for new columns | 6 |
| `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_aal/CMakeLists.txt` | No changes expected (same .c file) | - |

## Appendix B: Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| ACC100 mbuf leak if collect not called | Low | High (resource exhaustion) | Always collect on pipeline drain; add watchdog |
| MAC scheduler double-invocation | ~~Medium~~ **Eliminated** | ~~High~~ | Step 3.4 skips NR_slot_indication on pipeline hit → scheduler runs exactly once per slot |
| sched_response pool exhaustion | ~~High~~ **Eliminated** | ~~Critical~~ | Step 3.4 properly manages refcounting; pipeline sched_response deref'd in next tx_func |
| encode_mutex held across slots | Low | High (deadlock on error) | Add timeout to mutex; emergency release in error path |
| Pipeline stale data if slot skipped | Low | High (corrupted TX) | Frame/slot verification + sched_response cleanup in mismatch path |
| Double memory usage for buffers | N/A | Low (~19MB extra) | Acceptable for performance gain |
| HARQ retransmission with stale feedback | ~~Medium~~ **Eliminated** | ~~Medium~~ | k1 ≥ 6 slots: HARQ feedback not affected by 1-slot pipeline offset. Retransmissions safe to pipeline (same encoding path). See Phase 5 Step 5.1 |
| Encoding/codeword parameter mismatch | ~~High~~ **Eliminated** | ~~Critical~~ | Same msgDataTx used for encoding AND codeword phase (pipeline hit uses target_msgTx) |

## Appendix C: Rollback Strategy

Each phase is independently verifiable and can be rolled back:
- **Phase 1**: New API functions + double buffers + persistent params are additive; using buf_idx=0 with synchronous path is identical to original behavior
- **Phase 2**: Dual msgDataTx with index always 0 is identical to single pointer
- **Phase 3**: Pipeline state machine has synchronous fallback; disabling pipeline = always fallback
- **Phase 4**: Adds shutdown cleanup only; all runtime edge cases handled by Phase 3 fallback
- **Phase 5**: Analysis-only phase — validates HARQ timing and scheduler safety; no additional code changes beyond Phase 3
- **Phase 6**: Measurement instrumentation only — no behavior change if disabled

A single boolean `enable_encoding_pipeline` can control whether the pipeline is active or falls back to the synchronous path, making it safe to merge incrementally.

## Appendix D: Configuration

Add a runtime flag to enable/disable the pipeline:

```bash
# Enable pipeline (default OFF during development)
sudo ./nr-softmodem ... --enable-encoding-pipeline

# Disable pipeline (use synchronous path)
sudo ./nr-softmodem ...  # default: pipeline disabled
```

This can be implemented via `get_softmodem_params()` or a dedicated config parameter.
