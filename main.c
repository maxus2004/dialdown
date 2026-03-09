#define INPUT_DEVICE "pulse:alsa_output.usb-Apple__Inc._EarPods_GXLR4166L1-00.analog-stereo.monitor"
#define OUTPUT_DEVICE "pulse"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pty.h>
#include <alsa/asoundlib.h>
#include "kiss_fftr.h"
#include <stdbool.h>
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
#include <pthread.h>

#define APLITUDE 5000
#define SAMPLE_RATE 48000
#define FFT_SIZE 4
#define NUM_TONES 2
#define ZERO_FREQ 2000.0
#define ONE_FREQ 24000.0
#define TONES_PER_SECOND 12000
#define SAMPLES_PER_TONE (SAMPLE_RATE/TONES_PER_SECOND)
#define PI 3.14159265359

pthread_mutex_t serial_send_queue_mutex;
pthread_mutex_t audio_send_queue_mutex;

uint8_t *serial_send_queue = NULL;
uint8_t *audio_send_queue = NULL;

int serial_fd;

typedef struct {
    int bin_index;           // Pre-calculated FFT bin
    float frequency;         // Actual frequency
    float magnitude;         // Current magnitude
} tone_t;

tone_t tones[NUM_TONES];

void sleep_ms(int ms){
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = ms * 1000000;
    nanosleep(&req, NULL); 
}

void init_tones() {
    float freq_step = (ONE_FREQ - ZERO_FREQ) / (NUM_TONES - 1);
    
    for (int i = 0; i < NUM_TONES; i++) {
        tones[i].frequency = ZERO_FREQ + i * freq_step;
        tones[i].bin_index = (int)(tones[i].frequency * FFT_SIZE / SAMPLE_RATE + 0.5);
        tones[i].magnitude = 0;
        
        // printf("Tone %2d: %6.1f Hz -> bin %3d\n", 
        //        i, tones[i].frequency, tones[i].bin_index);
    }
}

int detect_tone_optimized(kiss_fft_cpx *fft_output) {
    int best_tone = -1;
    float max_mag = 0;
    
    // Only check our pre-calculated bins
    for (int t = 0; t < NUM_TONES; t++) {
        int bin = tones[t].bin_index;
        float mag = sqrt(fft_output[bin].r * fft_output[bin].r + 
                         fft_output[bin].i * fft_output[bin].i);
        tones[t].magnitude = mag;
        
        if (mag > max_mag) {
            max_mag = mag;
            best_tone = t;
        }
    }
    
    return best_tone;
}

void* audio_input_loop(void* args){
    snd_pcm_t *input_handle;
    snd_pcm_hw_params_t *input_params;
    short *input_buffer;
    float *fft_input;
    kiss_fft_cpx *fft_output;
    kiss_fftr_cfg fft_cfg;
    int err;
    
    // initialize tones array
    init_tones();

    // configure input device
    err = snd_pcm_open(&input_handle, INPUT_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(err));
    }
    
    snd_pcm_hw_params_alloca(&input_params);
    snd_pcm_hw_params_any(input_handle, input_params);
    snd_pcm_hw_params_set_access(input_handle, input_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(input_handle, input_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(input_handle, input_params, 1);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(input_handle, input_params, &rate, 0);

    // Set buffer to hold multiple FFT frames
    snd_pcm_uframes_t period_size = FFT_SIZE;
    snd_pcm_hw_params_set_period_size_near(input_handle, input_params, &period_size, 0);
    
    err = snd_pcm_hw_params(input_handle, input_params);
    if (err < 0) {
        fprintf(stderr, "Unable to set input device parameters: %s\n", snd_strerror(err));
    }
        
    // Allocate buffers
    input_buffer = malloc(FFT_SIZE * sizeof(short));
    fft_input = malloc(FFT_SIZE * sizeof(float));
    fft_output = malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    fft_cfg = kiss_fftr_alloc(FFT_SIZE, 0, NULL, NULL);
    

    bool recodring_byte = false;
    int current_bit = 0;
    uint8_t received_byte = 0;

    printf("audio input loop started\n");

    snd_pcm_prepare(input_handle);
    
    while (true) {
        snd_pcm_readi(input_handle, input_buffer, FFT_SIZE);
        
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_input[i] = (float)input_buffer[i];
        }

        kiss_fftr(fft_cfg, fft_input, fft_output);
        
        bool received_bit = detect_tone_optimized(fft_output);

        if(!recodring_byte && received_bit){
            recodring_byte = true;
            current_bit = 0;
        }else if(recodring_byte){
            received_byte = (received_byte>>1)|(received_bit<<7);
            current_bit++;
            if(current_bit >= 8){
                recodring_byte = false;
                pthread_mutex_lock(&serial_send_queue_mutex);
                arrpush(serial_send_queue, received_byte);
                pthread_mutex_unlock(&serial_send_queue_mutex);
            }
        }
    }

    return NULL;
}

void* audio_output_loop(void* args){
    printf("audio output loop started\n");

    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    int err;
    short *buffer;
    int buffer_frames = 64;
    int buffer_size;

    err = snd_pcm_open(&handle, OUTPUT_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "Unable to open pcm device: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);

    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, params, 1);

    unsigned int actual_rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(handle, params, &actual_rate, 0);

    snd_pcm_uframes_t hw_buffer_size = 64;
    snd_pcm_hw_params_set_buffer_size_near(handle, params, &hw_buffer_size);

    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        fprintf(stderr, "Unable to set hw parameters: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_pcm_prepare(handle);

    buffer_size = buffer_frames * 2; // 2 bytes per sample
    buffer = (short *)malloc(buffer_size);

    float phase = 0;
    float phase_increment_0 = 2 * PI * ZERO_FREQ / SAMPLE_RATE;
    float phase_increment_1 = 2 * PI * ONE_FREQ / SAMPLE_RATE;

    uint64_t current_sample = 0;
    bool sending = false;
    int data_length = 0;

    while (1) {
        for (int i = 0; i < buffer_frames; i++) {
            short sample = (short)(APLITUDE * sin(phase));
            buffer[i] = sample;
            if(!sending){
                pthread_mutex_lock(&audio_send_queue_mutex);
                data_length = arrlen(audio_send_queue);
                if(data_length == 0){
                    pthread_mutex_unlock(&audio_send_queue_mutex);
                }else{
                    current_sample = 0;
                    sending = true;
                }
            }
            if(sending){
                int bit = current_sample/SAMPLES_PER_TONE;
                if(bit >= 10*data_length){
                    sending = false;
                    phase += phase_increment_0;
                    arrsetlen(audio_send_queue, 0);
                    pthread_mutex_unlock(&audio_send_queue_mutex);
                }else{
                    if(bit%10 == 0){
                        phase += phase_increment_1;
                    }else if (bit%10 == 9){
                        phase += phase_increment_0;
                    }else if((audio_send_queue[bit/10]>>((bit%10)-1)&0x01)){
                        phase += phase_increment_1;
                    }else{
                        phase += phase_increment_0;
                    }
                }
                current_sample++;
            }else{
                current_sample = 0;
                phase += phase_increment_0;
            }
            
            if (phase >= 2 * PI) phase -= 2 * PI;
        }
        // ----------------------------------------------------------

        err = snd_pcm_writei(handle, buffer, buffer_frames);

        if (err == -EPIPE) {
            fprintf(stderr, "Underrun! Preparing device again.\n");
            snd_pcm_prepare(handle);
        } else if (err < 0) {
            fprintf(stderr, "Error from writei: %s\n", snd_strerror(err));
            break;
        } else if (err != buffer_frames) {
            fprintf(stderr, "Short write, wrote %d frames\n", err);
        }
    }

    return NULL;
}

void* serial_input_loop(void* args){
    printf("serial input loop started\n");

    char recive_buffer[1024];
    while(true){
        int n = read(serial_fd, recive_buffer, sizeof(recive_buffer) - 1);
        if (n > 0) {
            pthread_mutex_lock(&audio_send_queue_mutex);
            uint8_t* ptr = arraddnptr(audio_send_queue, n);
            memcpy(ptr, recive_buffer, n);
            pthread_mutex_unlock(&audio_send_queue_mutex);
        }
    }

    return NULL;
}

void* serial_output_loop(void* args){
    printf("serial output loop started\n");

    while(true){
        pthread_mutex_lock(&serial_send_queue_mutex);
        int len = arrlen(serial_send_queue);
        if(len == 0){
            pthread_mutex_unlock(&serial_send_queue_mutex);
            sleep_ms(10);
            continue;
        }
        write(serial_fd, serial_send_queue, len);
        arrsetlen(serial_send_queue, 0);
        pthread_mutex_unlock(&serial_send_queue_mutex);
    }

    return NULL;
}

int main() {
    pthread_mutex_init(&serial_send_queue_mutex, NULL);
    pthread_mutex_init(&audio_send_queue_mutex, NULL);

    int serial_slave_fd;
    char serial_name[256];
    if (openpty(&serial_fd, &serial_slave_fd, serial_name, NULL, NULL) == -1) {
        perror("openpty failed");
        exit(1);
    }
    printf("created serial port: %s\n", serial_name);

    pthread_t audio_input_thread;
    if (pthread_create(&audio_input_thread, NULL, audio_input_loop, NULL) != 0) {
        fprintf(stderr, "Error creating audio_input_thread\n");
    }
    pthread_t serial_output_thread;
    if (pthread_create(&serial_output_thread, NULL, serial_output_loop, NULL) != 0) {
        fprintf(stderr, "Error creating serial_output_thread\n");
    }
    pthread_t audio_output_thread;
    if (pthread_create(&audio_output_thread, NULL, audio_output_loop, NULL) != 0) {
        fprintf(stderr, "Error creating audio_output_thread\n");
    }
    pthread_t serial_input_thread;
    if (pthread_create(&serial_input_thread, NULL, serial_input_loop, NULL) != 0) {
        fprintf(stderr, "Error creating serial_input_thread\n");
    }

    sleep_ms(50);

    printf("commands:\n");
    printf(" - exit\n");

    while(true){
        char line[128];
        printf("> ");
        fflush(stdout);
        if (fgets(line, 128, stdin) != NULL) {
            line[strcspn(line, "\n")] = 0;
            if(strcmp("exit",line)==0){
                break;
            }
        } else {
            perror("Error reading console input");
        }
    }

    return 0;
}