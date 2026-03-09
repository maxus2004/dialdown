#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SOUND_CARD_DEVICE "pulse:alsa_output.platform-snd_aloop.0.analog-stereo"
#define SAMPLE_RATE 48000
#define CHANNELS 1
#define FORMAT SND_PCM_FORMAT_S16_LE

int main() {
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    int err;
    short *buffer;
    int buffer_frames = 1024; // Number of frames per buffer
    int buffer_size;

    // 1. Open the PCM device for playback (writing) on the loopback device
    err = snd_pcm_open(&handle, SOUND_CARD_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "Unable to open pcm device: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    // 2. Allocate and initialize hardware parameters object
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);

    // 3. Set hardware parameters
    // Interleaved mode (left, right, left, right, ...)
    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) { /* handle error */ }

    // Set format (e.g., S16_LE)
    err = snd_pcm_hw_params_set_format(handle, params, FORMAT);
    if (err < 0) { /* handle error */ }

    // Set number of channels
    err = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (err < 0) { /* handle error */ }

    // Set sample rate
    unsigned int actual_rate = SAMPLE_RATE;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &actual_rate, 0);
    if (err < 0) { /* handle error */ }
    printf("Sample rate set to: %d\n", actual_rate);

    // 4. Write parameters to the driver
    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        fprintf(stderr, "Unable to set hw parameters: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    // 5. Prepare the device for writing
    snd_pcm_prepare(handle);

    // 6. Allocate a buffer (size in bytes: frames * channels * sample_size)
    // For S16_LE, sample size is 2 bytes.
    buffer_size = buffer_frames * CHANNELS * 2; // 2 bytes per sample
    buffer = (short *)malloc(buffer_size);

    // 7. Generate and write audio data in a loop
    printf("Starting audio stream. Press Ctrl+C to stop.\n");
    float amplitude = 30000; // Volume
    float freq = 440.0; // 440 Hz tone (A4)
    float phase = 0;
    float phase_increment = 2 * M_PI * freq / SAMPLE_RATE;

    while (1) {
        // --- Replace this block with your own audio generation ---
        // Example: Generate a simple sine wave
        for (int i = 0; i < buffer_frames; i++) {
            short sample = (short)(amplitude * sin(phase));
            buffer[i] = sample;
            phase += phase_increment;
            if (phase >= 2 * M_PI) phase -= 2 * M_PI;
        }
        // ----------------------------------------------------------

        // Write the audio data to the loopback device
        err = snd_pcm_writei(handle, buffer, buffer_frames);
        printf("AUDIO SENT!!!\n");
        if (err == -EPIPE) {
            // Underrun occurred (we wrote data too slowly)
            fprintf(stderr, "Underrun! Preparing device again.\n");
            snd_pcm_prepare(handle);
        } else if (err < 0) {
            fprintf(stderr, "Error from writei: %s\n", snd_strerror(err));
            break;
        } else if (err != buffer_frames) {
            fprintf(stderr, "Short write, wrote %d frames\n", err);
        }
    }

    // 8. Cleanup (this part won't be reached in this example due to infinite loop)
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);
    return 0;
}