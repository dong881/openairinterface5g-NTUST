/*
 * Multi-Stream ACC100 Hardware Acceleration Implementation
 * Removes mutex bottlenecks with robust fallback mechanisms
 */

#define _POSIX_C_SOURCE 199309L
#include "nrLDPC_coding_aal_multistream.h"
#include "common/utils/LOG/log.h"
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

// Global multi-stream pool
acc100_stream_pool_t acc100_pool = {0};

// External references to original functions for fallback
extern int32_t nrLDPC_coding_encoder_original(nrLDPC_slot_encoding_parameters_t *params);
extern int32_t nrLDPC_coding_decoder_original(nrLDPC_slot_decoding_parameters_t *params);

// Utility function to get current timestamp
static uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Mode management functions
acc100_multistream_mode_t acc100_get_current_mode(void) {
    return atomic_load(&acc100_pool.mode);
}

const char* acc100_mode_to_string(acc100_multistream_mode_t mode) {
    switch (mode) {
        case ACC100_MULTISTREAM_ENABLED:  return "MULTISTREAM_ENABLED";
        case ACC100_MULTISTREAM_DEGRADED: return "MULTISTREAM_DEGRADED";
        case ACC100_MULTISTREAM_FALLBACK: return "MULTISTREAM_FALLBACK";
        case ACC100_MULTISTREAM_DISABLED: return "MULTISTREAM_DISABLED";
        default: return "UNKNOWN";
    }
}

// Fallback decision logic
bool acc100_should_use_multistream(void) {
    acc100_multistream_mode_t mode = acc100_get_current_mode();

    switch (mode) {
        case ACC100_MULTISTREAM_ENABLED:
        case ACC100_MULTISTREAM_DEGRADED:
            return true;
        case ACC100_MULTISTREAM_FALLBACK:
        case ACC100_MULTISTREAM_DISABLED:
        default:
            return false;
    }
}

// Fallback trigger function
void acc100_trigger_fallback(const char *reason) {
    acc100_multistream_mode_t current_mode = acc100_get_current_mode();

    if (current_mode == ACC100_MULTISTREAM_FALLBACK ||
        current_mode == ACC100_MULTISTREAM_DISABLED) {
        return; // Already in fallback
    }

    LOG_W(PHY, "ACC100 Multi-stream triggering fallback: %s\n", reason);

    // Update mode to fallback
    atomic_store(&acc100_pool.mode, ACC100_MULTISTREAM_FALLBACK);
    acc100_pool.fallback_timestamp = get_timestamp_ns();
    acc100_pool.total_failures++;

    // Initialize fallback resources if not done
    if (!acc100_pool.fallback_resources_initialized) {
        pthread_mutex_init(&acc100_pool.fallback_encode_mutex, NULL);
        pthread_mutex_init(&acc100_pool.fallback_decode_mutex, NULL);
        acc100_pool.fallback_resources_initialized = true;
        LOG_I(PHY, "ACC100 fallback resources initialized\n");
    }
}

// Main fallback-aware encoding function
int32_t acc100_multistream_encode_with_fallback(nrLDPC_slot_encoding_parameters_t *params) {
    // Check if we should use multi-stream or fallback
    if (!acc100_should_use_multistream()) {
        return acc100_fallback_encoder(params);
    }

    // Try multi-stream encoding
    acc100_stream_t *stream = acc100_acquire_encode_stream();
    if (!stream) {
        // No streams available, fallback
        acc100_trigger_fallback("No encode streams available");
        return acc100_fallback_encoder(params);
    }

    // Attempt multi-stream encoding
    int32_t result = acc100_multistream_encoder(params, stream);

    if (result != 0) {
        // Encoding failed
        acc100_mark_stream_failed(stream, "Encoding operation failed");
        acc100_release_stream(stream);

        // Check if we should trigger fallback
        if (acc100_pool.total_failures >= MULTISTREAM_MAX_FAILURES) {
            acc100_trigger_fallback("Too many encoding failures");
            return acc100_fallback_encoder(params);
        }

        // Try another stream
        stream = acc100_acquire_encode_stream();
        if (!stream) {
            acc100_trigger_fallback("No backup encode streams available");
            return acc100_fallback_encoder(params);
        }

        result = acc100_multistream_encoder(params, stream);
        if (result != 0) {
            acc100_mark_stream_failed(stream, "Backup encoding failed");
            acc100_trigger_fallback("Multiple encode stream failures");
            acc100_release_stream(stream);
            return acc100_fallback_encoder(params);
        }
    }

    acc100_release_stream(stream);
    return result;
}

// Main fallback-aware decoding function
int32_t acc100_multistream_decode_with_fallback(nrLDPC_slot_decoding_parameters_t *params) {
    // Check if we should use multi-stream or fallback
    if (!acc100_should_use_multistream()) {
        return acc100_fallback_decoder(params);
    }

    // Try multi-stream decoding
    acc100_stream_t *stream = acc100_acquire_decode_stream();
    if (!stream) {
        // No streams available, fallback
        acc100_trigger_fallback("No decode streams available");
        return acc100_fallback_decoder(params);
    }

    // Attempt multi-stream decoding
    int32_t result = acc100_multistream_decoder(params, stream);

    if (result != 0) {
        // Decoding failed
        acc100_mark_stream_failed(stream, "Decoding operation failed");
        acc100_release_stream(stream);

        // Check if we should trigger fallback
        if (acc100_pool.total_failures >= MULTISTREAM_MAX_FAILURES) {
            acc100_trigger_fallback("Too many decoding failures");
            return acc100_fallback_decoder(params);
        }

        // Try another stream
        stream = acc100_acquire_decode_stream();
        if (!stream) {
            acc100_trigger_fallback("No backup decode streams available");
            return acc100_fallback_decoder(params);
        }

        result = acc100_multistream_decoder(params, stream);
        if (result != 0) {
            acc100_mark_stream_failed(stream, "Backup decoding failed");
            acc100_trigger_fallback("Multiple decode stream failures");
            acc100_release_stream(stream);
            return acc100_fallback_decoder(params);
        }
    }

    acc100_release_stream(stream);
    return result;
}

// Fallback encoder using original single-stream with mutex
int32_t acc100_fallback_encoder(nrLDPC_slot_encoding_parameters_t *params) {
    if (!acc100_pool.fallback_resources_initialized) {
        // Emergency fallback - call original function directly
        LOG_W(PHY, "ACC100 emergency fallback: calling original encoder\n");
        return nrLDPC_coding_encoder_original(params);
    }

    // Use fallback mutex to serialize access (original behavior)
    pthread_mutex_lock(&acc100_pool.fallback_encode_mutex);
    int32_t result = nrLDPC_coding_encoder_original(params);
    pthread_mutex_unlock(&acc100_pool.fallback_encode_mutex);

    return result;
}

// Fallback decoder using original single-stream with mutex
int32_t acc100_fallback_decoder(nrLDPC_slot_decoding_parameters_t *params) {
    if (!acc100_pool.fallback_resources_initialized) {
        // Emergency fallback - call original function directly
        LOG_W(PHY, "ACC100 emergency fallback: calling original decoder\n");
        return nrLDPC_coding_decoder_original(params);
    }

    // Use fallback mutex to serialize access (original behavior)
    pthread_mutex_lock(&acc100_pool.fallback_decode_mutex);
    int32_t result = nrLDPC_coding_decoder_original(params);
    pthread_mutex_unlock(&acc100_pool.fallback_decode_mutex);

    return result;
}

// Stream failure handling
void acc100_mark_stream_failed(acc100_stream_t *stream, const char *reason) {
    if (!stream) return;

    pthread_mutex_lock(&stream->stream_mutex);

    stream->consecutive_failures++;
    stream->total_failures++;
    stream->last_failure_timestamp = get_timestamp_ns();

    LOG_W(PHY, "ACC100 Stream %d failed: %s (consecutive: %d, total: %ld)\n",
          stream->stream_id, reason, stream->consecutive_failures, stream->total_failures);

    // Disable stream if too many consecutive failures
    if (stream->consecutive_failures >= MULTISTREAM_MAX_FAILURES) {
        atomic_store(&stream->state, ACC100_STREAM_DISABLED);
        acc100_pool.healthy_streams--;
        LOG_E(PHY, "ACC100 Stream %d disabled due to consecutive failures\n", stream->stream_id);
    } else {
        atomic_store(&stream->state, ACC100_STREAM_ERROR);
    }

    pthread_mutex_unlock(&stream->stream_mutex);

    // Update pool failure count
    acc100_pool.total_failures++;
}

// Stream health check
bool acc100_stream_is_healthy(acc100_stream_t *stream) {
    if (!stream) return false;

    acc100_stream_state_t state = atomic_load(&stream->state);
    return (state == ACC100_STREAM_IDLE);
}

// Stream acquisition with timeout and fallback
acc100_stream_t* acc100_acquire_encode_stream(void) {
    uint64_t start_time = get_timestamp_ns();
    uint64_t timeout_ns = MULTISTREAM_FALLBACK_TIMEOUT_MS * 1000000ULL;

    while ((get_timestamp_ns() - start_time) < timeout_ns) {
        uint32_t stream_idx = atomic_fetch_add(&acc100_pool.next_stream_enc, 1) % acc100_pool.total_streams;

        // Find the actual stream across all devices
        for (int dev = 0; dev < acc100_pool.nb_devices; dev++) {
            for (int stream = 0; stream < acc100_pool.devices[dev].nb_streams; stream++) {
                acc100_stream_t *s = &acc100_pool.devices[dev].streams[stream];

                if (s->stream_id == stream_idx && acc100_stream_is_healthy(s)) {
                    acc100_stream_state_t expected = ACC100_STREAM_IDLE;
                    if (atomic_compare_exchange_strong(&s->state, &expected, ACC100_STREAM_ENCODING)) {
                        return s;
                    }
                }
            }
        }

        // Brief backoff before retrying
        usleep(100); // 100 microseconds
    }

    LOG_W(PHY, "ACC100 encode stream acquisition timed out after %d ms\n", MULTISTREAM_FALLBACK_TIMEOUT_MS);
    return NULL;
}

// Similar implementation for decode stream acquisition
acc100_stream_t* acc100_acquire_decode_stream(void) {
    uint64_t start_time = get_timestamp_ns();
    uint64_t timeout_ns = MULTISTREAM_FALLBACK_TIMEOUT_MS * 1000000ULL;

    while ((get_timestamp_ns() - start_time) < timeout_ns) {
        uint32_t stream_idx = atomic_fetch_add(&acc100_pool.next_stream_dec, 1) % acc100_pool.total_streams;

        // Find the actual stream across all devices
        for (int dev = 0; dev < acc100_pool.nb_devices; dev++) {
            for (int stream = 0; stream < acc100_pool.devices[dev].nb_streams; stream++) {
                acc100_stream_t *s = &acc100_pool.devices[dev].streams[stream];

                if (s->stream_id == stream_idx && acc100_stream_is_healthy(s)) {
                    acc100_stream_state_t expected = ACC100_STREAM_IDLE;
                    if (atomic_compare_exchange_strong(&s->state, &expected, ACC100_STREAM_DECODING)) {
                        return s;
                    }
                }
            }
        }

        usleep(100); // 100 microseconds
    }

    LOG_W(PHY, "ACC100 decode stream acquisition timed out after %d ms\n", MULTISTREAM_FALLBACK_TIMEOUT_MS);
    return NULL;
}

// Stream release
void acc100_release_stream(acc100_stream_t *stream) {
    if (!stream) return;

    stream->last_used_timestamp = get_timestamp_ns();
    stream->operations_completed++;

    // Reset consecutive failures on successful operation
    if (stream->consecutive_failures > 0) {
        pthread_mutex_lock(&stream->stream_mutex);
        stream->consecutive_failures = 0;
        pthread_mutex_unlock(&stream->stream_mutex);
    }

    atomic_store(&stream->state, ACC100_STREAM_IDLE);
}

// Placeholder implementations for actual stream operations
// These would contain the real DPDK/ACC100 logic
int32_t acc100_multistream_encoder(nrLDPC_slot_encoding_parameters_t *params, acc100_stream_t *stream) {
    (void)params; // Suppress unused parameter warning
    // TODO: Implement actual multi-stream encoding with stream-specific resources
    // For now, return success for framework testing
    LOG_D(PHY, "ACC100 multi-stream encoding on stream %d\n", stream->stream_id);
    return 0;
}

int32_t acc100_multistream_decoder(nrLDPC_slot_decoding_parameters_t *params, acc100_stream_t *stream) {
    (void)params; // Suppress unused parameter warning
    // TODO: Implement actual multi-stream decoding with stream-specific resources
    // For now, return success for framework testing
    LOG_D(PHY, "ACC100 multi-stream decoding on stream %d\n", stream->stream_id);
    return 0;
}

// Initialization and cleanup functions
int32_t acc100_multistream_init(void) {
    pthread_mutex_lock(&acc100_pool.pool_init_mutex);

    if (acc100_pool.pool_initialized) {
        pthread_mutex_unlock(&acc100_pool.pool_init_mutex);
        return 0;
    }

    // Initialize pool state
    atomic_store(&acc100_pool.mode, ACC100_MULTISTREAM_ENABLED);
    atomic_store(&acc100_pool.next_stream_enc, 0);
    atomic_store(&acc100_pool.next_stream_dec, 0);

    // TODO: Initialize actual ACC100 devices and streams
    // For now, set up minimal configuration for testing
    acc100_pool.nb_devices = 1;
    acc100_pool.total_streams = 4;
    acc100_pool.healthy_streams = 4;

    acc100_pool.pool_initialized = true;
    pthread_mutex_unlock(&acc100_pool.pool_init_mutex);

    LOG_I(PHY, "ACC100 multi-stream pool initialized with %d streams\n", acc100_pool.total_streams);
    return 0;
}

void acc100_multistream_cleanup(void) {
    // TODO: Cleanup DPDK resources and streams

    if (acc100_pool.fallback_resources_initialized) {
        pthread_mutex_destroy(&acc100_pool.fallback_encode_mutex);
        pthread_mutex_destroy(&acc100_pool.fallback_decode_mutex);
    }

    pthread_mutex_destroy(&acc100_pool.pool_init_mutex);
    memset(&acc100_pool, 0, sizeof(acc100_pool));

    LOG_I(PHY, "ACC100 multi-stream pool cleaned up\n");
}

// Statistics and monitoring
void acc100_print_multistream_stats(void) {
    printf("\n=== ACC100 Multi-Stream Statistics ===\n");
    printf("Mode: %s\n", acc100_mode_to_string(acc100_get_current_mode()));
    printf("Total Streams: %d\n", acc100_pool.total_streams);
    printf("Healthy Streams: %d\n", acc100_pool.healthy_streams);
    printf("Total Failures: %d\n", acc100_pool.total_failures);
    printf("Encode Operations: %ld\n", atomic_load(&acc100_pool.encode_operations));
    printf("Decode Operations: %ld\n", atomic_load(&acc100_pool.decode_operations));

    if (acc100_pool.fallback_timestamp > 0) {
        printf("Fallback Active Since: %ld ns ago\n", get_timestamp_ns() - acc100_pool.fallback_timestamp);
    }

    printf("======================================\n\n");
}