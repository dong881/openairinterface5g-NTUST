/*
 * ACC100 Multi-Stream Integration with Fallback
 * Replaces the original mutex-based implementation
 */

#include "nrLDPC_coding_aal_multistream.h"
#include "common/utils/LOG/log.h"

// Configuration flag to enable/disable multi-stream mode
static bool multistream_enabled = true;

// Original function prototypes (preserved for fallback)
int32_t nrLDPC_coding_encoder_original(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters);
int32_t nrLDPC_coding_decoder_original(nrLDPC_slot_decoding_parameters_t *nrLDPC_slot_decoding_parameters);

// New initialization function that replaces original
int32_t nrLDPC_coding_init(void)
{
    LOG_I(PHY, "Initializing ACC100 with multi-stream support\n");

    // Try to initialize multi-stream support
    if (multistream_enabled) {
        int32_t result = acc100_multistream_init();
        if (result == 0) {
            LOG_I(PHY, "ACC100 multi-stream initialization successful\n");
            return 0;
        } else {
            LOG_W(PHY, "ACC100 multi-stream initialization failed, falling back to original\n");
            multistream_enabled = false;
        }
    }

    // Fallback to original initialization
    LOG_I(PHY, "Using original ACC100 single-stream mode\n");

    // Call original initialization code
    extern pthread_mutex_t encode_mutex, decode_mutex;
    pthread_mutex_init(&encode_mutex, NULL);
    pthread_mutex_init(&decode_mutex, NULL);

    // TODO: Add original ACC100 device initialization here
    // This would include the original device discovery and setup code

    return 0;
}

// New encoder function that replaces original with multi-stream + fallback
int32_t nrLDPC_coding_encoder(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters)
{
    // Try multi-stream first if enabled
    if (multistream_enabled && acc100_should_use_multistream()) {
        int32_t result = acc100_multistream_encode_with_fallback(nrLDPC_slot_encoding_parameters);

        // If multi-stream succeeds, we're done
        if (result == 0) {
            return 0;
        }

        // If multi-stream fails consistently, disable it
        if (acc100_get_current_mode() == ACC100_MULTISTREAM_DISABLED) {
            LOG_W(PHY, "ACC100 multi-stream disabled, switching to original encoder permanently\n");
            multistream_enabled = false;
        }
    }

    // Fallback to original encoder
    LOG_D(PHY, "Using original ACC100 encoder with mutex\n");
    return nrLDPC_coding_encoder_original(nrLDPC_slot_encoding_parameters);
}

// New decoder function that replaces original with multi-stream + fallback
int32_t nrLDPC_coding_decoder(nrLDPC_slot_decoding_parameters_t *nrLDPC_slot_decoding_parameters)
{
    // Try multi-stream first if enabled
    if (multistream_enabled && acc100_should_use_multistream()) {
        int32_t result = acc100_multistream_decode_with_fallback(nrLDPC_slot_decoding_parameters);

        // If multi-stream succeeds, we're done
        if (result == 0) {
            return 0;
        }

        // If multi-stream fails consistently, disable it
        if (acc100_get_current_mode() == ACC100_MULTISTREAM_DISABLED) {
            LOG_W(PHY, "ACC100 multi-stream disabled, switching to original decoder permanently\n");
            multistream_enabled = false;
        }
    }

    // Fallback to original decoder
    LOG_D(PHY, "Using original ACC100 decoder with mutex\n");
    return nrLDPC_coding_decoder_original(nrLDPC_slot_decoding_parameters);
}

// Cleanup function
void nrLDPC_coding_cleanup(void)
{
    if (multistream_enabled) {
        acc100_multistream_cleanup();
    }

    // Cleanup original resources if they were initialized
    extern pthread_mutex_t encode_mutex, decode_mutex;
    pthread_mutex_destroy(&encode_mutex);
    pthread_mutex_destroy(&decode_mutex);

    LOG_I(PHY, "ACC100 resources cleaned up\n");
}

// Performance monitoring function
void nrLDPC_coding_print_stats(void)
{
    if (multistream_enabled && acc100_should_use_multistream()) {
        acc100_print_multistream_stats();
    } else {
        printf("ACC100 running in original single-stream mode\n");
    }
}

// Runtime mode switching (for testing/debugging)
void nrLDPC_coding_enable_multistream(bool enable)
{
    if (enable && !multistream_enabled) {
        LOG_I(PHY, "Attempting to re-enable ACC100 multi-stream mode\n");
        if (acc100_multistream_init() == 0) {
            multistream_enabled = true;
            LOG_I(PHY, "ACC100 multi-stream mode enabled\n");
        } else {
            LOG_W(PHY, "Failed to enable ACC100 multi-stream mode\n");
        }
    } else if (!enable && multistream_enabled) {
        LOG_I(PHY, "Disabling ACC100 multi-stream mode\n");
        multistream_enabled = false;
    }
}

bool nrLDPC_coding_is_multistream_enabled(void)
{
    return multistream_enabled && acc100_should_use_multistream();
}

// Health check function (can be called periodically)
void nrLDPC_coding_health_check(void)
{
    if (!multistream_enabled) {
        return;
    }

    acc100_multistream_mode_t mode = acc100_get_current_mode();

    switch (mode) {
        case ACC100_MULTISTREAM_ENABLED:
            LOG_D(PHY, "ACC100 multi-stream: All streams healthy\n");
            break;

        case ACC100_MULTISTREAM_DEGRADED:
            LOG_W(PHY, "ACC100 multi-stream: Some streams failed, running in degraded mode\n");
            acc100_check_and_recover_streams();
            break;

        case ACC100_MULTISTREAM_FALLBACK:
            LOG_W(PHY, "ACC100 multi-stream: Running in fallback mode\n");
            // Attempt recovery after some time
            acc100_check_and_recover_streams();
            break;

        case ACC100_MULTISTREAM_DISABLED:
            LOG_E(PHY, "ACC100 multi-stream: Permanently disabled due to failures\n");
            multistream_enabled = false;
            break;
    }
}