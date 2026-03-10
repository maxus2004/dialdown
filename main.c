#define INPUT_DEVICE "pulse:ReceiverMicrophone"
#define OUTPUT_DEVICE "pulse:TransmitterSpeaker"

#include <signal.h>
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

#define AMPLITUDE 15000
#define SAMPLE_RATE 48000
#define FFT_SIZE 64
#define FIRST_SUBCARRIER 4
#define SUBCARRIER_COUNT 16
#define SUBCARRIER_SPACING 1
#define SUBCARRIER_SYMBOLS_COUNT 4
#define CYCLIC_PREFIX 16
#define FRAME_SPACING 2
#define CORRELATION_THRESHOLD 0.05
#define CORRELATION_FALLOFF_THRESHOLD 0.01
#define CORRELATION_OFFSET (FFT_SIZE-1)


#define BITS_PER_SYMBOL (SUBCARRIER_COUNT*(int)log2(SUBCARRIER_SYMBOLS_COUNT))
#define PI 3.14159265359

kiss_fft_cpx subcarrier_symbols[SUBCARRIER_SYMBOLS_COUNT];

kiss_fft_cpx default_symbol = {0.5,0};
kiss_fft_cpx preamble_symbol = {1,0};
kiss_fft_cpx after_preamble_symbol = {0,0};


pthread_mutex_t serial_send_queue_mutex;
pthread_mutex_t audio_send_queue_mutex;

uint8_t *serial_send_queue = NULL;
uint8_t *audio_send_queue = NULL;

uint32_t audio_devs[4];

int serial_fd;

void sleep_ms(int ms){
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = ms * 1000000;
    nanosleep(&req, NULL); 
}

float correlation(int16_t *a, int16_t *b, int len){
    float sum = 0;
    for(int i = 0;i<len;i++){
        sum += a[i]*b[i]*0.00003*0.00003;
    }
    return sum/FFT_SIZE*SUBCARRIER_COUNT;
}

void* audio_input_loop(void* args){
    snd_pcm_t *input_handle;
    snd_pcm_hw_params_t *input_params;
    short garbage[65535];
    short input_buffer[FFT_SIZE];
    float fft_input[FFT_SIZE];
    kiss_fft_cpx fft_output[FFT_SIZE];
    kiss_fftr_cfg fft_cfg = kiss_fftr_alloc(FFT_SIZE, false, NULL, NULL);
    int err;

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
        
    printf("audio input loop started\n");

    snd_pcm_prepare(input_handle);
    
    while (true) {

        //waiting for preamble
        short *sliding_buffer = NULL;
        arrsetlen(sliding_buffer, FFT_SIZE*2);
        snd_pcm_readi(input_handle, sliding_buffer, FFT_SIZE*2);
        while(true){
            arrdel(sliding_buffer, 0);
            arraddn(sliding_buffer, 1);
            snd_pcm_readi(input_handle, &sliding_buffer[FFT_SIZE*2-1], 1);
            float max_c = correlation(sliding_buffer, sliding_buffer+FFT_SIZE, FFT_SIZE);
            if(max_c > CORRELATION_THRESHOLD){
                while(true){
                    arrdel(sliding_buffer, 0);
                    arraddn(sliding_buffer, 1);
                    snd_pcm_readi(input_handle, &sliding_buffer[FFT_SIZE*2-1], 1);
                    float c = correlation(sliding_buffer, sliding_buffer+FFT_SIZE, FFT_SIZE);
                    printf("buffer: \n");
                    for(int j = 0;j<arrlen(sliding_buffer);j++){
                        printf("%i ",sliding_buffer[j]);
                    }
                    printf("\n");
                    printf("correlation: %f\n", c);
                    if(c > max_c){
                        max_c = c;
                    }else if (c <= max_c-CORRELATION_FALLOFF_THRESHOLD){
                        goto peak_found;
                    }
                }
            }
        }
        peak_found:
        printf("found preamble\n");
        for (int i = 0; i < FFT_SIZE; i++) {
                fft_input[i] = (float)sliding_buffer[i];
            }
        kiss_fftr(fft_cfg, fft_input, fft_output);
        float received_volume = sqrt(fft_output[FIRST_SUBCARRIER].r*fft_output[FIRST_SUBCARRIER].r+fft_output[FIRST_SUBCARRIER].i*fft_output[FIRST_SUBCARRIER].i);
        printf("received_volume: %f\n",received_volume);
        
        //receive data

        int received_symbols = 0;
        int message_length = 5;
        int received_bits = 0;
        bool message_length_set = false;
        uint8_t message[65536] = {0};
        while(true){
            snd_pcm_readi(input_handle, garbage, CYCLIC_PREFIX/2);
            if(received_symbols==0 && CORRELATION_OFFSET>0){
                snd_pcm_readi(input_handle, garbage, CORRELATION_OFFSET);
                snd_pcm_readi(input_handle, input_buffer, FFT_SIZE);
            }else{
                snd_pcm_readi(input_handle, input_buffer, FFT_SIZE);
            }
            snd_pcm_readi(input_handle, garbage, CYCLIC_PREFIX/2);
            for (int i = 0; i < FFT_SIZE; i++) {
                fft_input[i] = (float)input_buffer[i];
            }

            printf("buffer: \n");
            for(int j = 0;j<FFT_SIZE;j++){
                printf("%i ",input_buffer[j]);
            }
            printf("\n");

            kiss_fftr(fft_cfg, fft_input, fft_output);

            kiss_fft_cpx ofdm_symbol[SUBCARRIER_COUNT];
            for(int i = 0;i<SUBCARRIER_COUNT;i++){
                ofdm_symbol[i] = fft_output[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING];
                float value = sqrt(ofdm_symbol[i].r*ofdm_symbol[i].r+ofdm_symbol[i].i*ofdm_symbol[i].i)/received_volume;
                float min_diff = 10;
                int best_symbol = 0;
                printf("value: %f\n", value);
                for(int j = 0;j<SUBCARRIER_SYMBOLS_COUNT;j++){
                    float diff = fabsf(value-subcarrier_symbols[j].r);
                    if(diff<min_diff){
                        min_diff = diff;
                        best_symbol = j;
                    }
                }
                message[received_bits/8] |= best_symbol<<(received_bits%8);
                printf("best symbol: ");
                for(int j = 0;j<(int)log2(SUBCARRIER_SYMBOLS_COUNT);j++){
                    printf("%i",(best_symbol>>j)%2);
                }
                printf("\n");
                received_bits+=(int)log2(SUBCARRIER_SYMBOLS_COUNT);
                if(received_bits>=32 && !message_length_set){
                    message_length_set = true;
                    int length1 = ((uint16_t*)message)[0];
                    int length2 = ((uint16_t*)message)[1];
                    if(length1!=length2 || length1>1600){
                        message_length = 4;
                        printf("wrong length!\n");
                    }else{
                        message_length = *((uint16_t*)message);
                        printf("message length: %i\n", message[0]);
                    }
                }
            }

            received_symbols++;

            if(received_symbols > message_length*8/BITS_PER_SYMBOL)break;
        }
        printf("message finished\n");

        if(message_length > 4){
            pthread_mutex_lock(&serial_send_queue_mutex);
            uint8_t *ptr = arraddnptr(serial_send_queue,message_length-4);
            memcpy(ptr, message+4, message_length-4);
            pthread_mutex_unlock(&serial_send_queue_mutex);
        }
    }

    return NULL;
}

void play_audio_samples_float(snd_pcm_t *handle, float *ifft_samples, int len){
    short output_buffer[len];

    for (int i = 0; i < len; i++) {
        output_buffer[i] = (short)(ifft_samples[i]/2/SUBCARRIER_COUNT*AMPLITUDE);
    }

    int err = snd_pcm_writei(handle, output_buffer, len);

    if (err == -EPIPE) {
        fprintf(stderr, "Underrun! Preparing device again.\n");
        snd_pcm_prepare(handle);
    } else if (err < 0) {
        fprintf(stderr, "Error from writei: %s\n", snd_strerror(err));
    } else if (err != len) {
        fprintf(stderr, "Short write, wrote %d frames\n", err);
    }
}

void* audio_output_loop(void* args){
    printf("audio output loop started\n");

    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    int err;

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

    snd_pcm_uframes_t hw_buffer_size = 1024;
    snd_pcm_hw_params_set_buffer_size_near(handle, params, &hw_buffer_size);

    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        fprintf(stderr, "Unable to set hw parameters: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_pcm_prepare(handle);

    kiss_fft_cpx ifft_input[FFT_SIZE];
    float ifft_output[FFT_SIZE];
    kiss_fftr_cfg ifft_cfg = kiss_fftr_alloc(FFT_SIZE, true, NULL, NULL);

    while (1) {

        pthread_mutex_lock(&audio_send_queue_mutex);
        if(arrlen(audio_send_queue) > 0){
            //make frame
            int frame_length = arrlen(audio_send_queue)+4;
            uint8_t frame[frame_length];
            ((uint16_t*)frame)[0] = frame_length;
            ((uint16_t*)frame)[1] = frame_length;
            memcpy(frame+4,audio_send_queue,arrlen(audio_send_queue));
            arrsetlen(audio_send_queue, 0);
            pthread_mutex_unlock(&audio_send_queue_mutex);

            //send preamble 
            memset(ifft_input, 0, sizeof(ifft_input));
            for(int i = 0;i<1;i++){
                ifft_input[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING] = preamble_symbol;
            }
            kiss_fftri(ifft_cfg, ifft_input, ifft_output);
            play_audio_samples_float(handle, ifft_output, FFT_SIZE);
            play_audio_samples_float(handle, ifft_output, FFT_SIZE);
            memset(ifft_input, 0, sizeof(ifft_input));
            for(int i = 0;i<SUBCARRIER_COUNT;i++){
                ifft_input[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING] = after_preamble_symbol;
            }
            kiss_fftri(ifft_cfg, ifft_input, ifft_output);
            play_audio_samples_float(handle, ifft_output, FFT_SIZE);

            //send frame
            for(int current_symbol = 0; current_symbol <= frame_length*8/BITS_PER_SYMBOL; current_symbol++){
                memset(ifft_input, 0, sizeof(ifft_input));
                kiss_fft_cpx ofdm_symbol[SUBCARRIER_COUNT];
                int first_bit = current_symbol*BITS_PER_SYMBOL;
                for(int subcarrier = 0;subcarrier<SUBCARRIER_COUNT;subcarrier++){
                    int bit = first_bit+subcarrier*log2(SUBCARRIER_SYMBOLS_COUNT);
                    if(bit/8<frame_length){
                        int bits = (frame[bit/8]>>(bit%8))%SUBCARRIER_SYMBOLS_COUNT;
                        ofdm_symbol[subcarrier] = subcarrier_symbols[bits];
                    }else{
                        ofdm_symbol[subcarrier] = default_symbol;
                    }
                }

                for(int i = 0;i<SUBCARRIER_COUNT;i++){
                    ifft_input[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING] = ofdm_symbol[i];
                }

                kiss_fftri(ifft_cfg, ifft_input, ifft_output);
                play_audio_samples_float(handle, ifft_output+FFT_SIZE-CYCLIC_PREFIX, CYCLIC_PREFIX);
                play_audio_samples_float(handle, ifft_output, FFT_SIZE);
            }

            for(int a = 0;a<FRAME_SPACING;a++){
                memset(ifft_input, 0, sizeof(ifft_input));
                pthread_mutex_unlock(&audio_send_queue_mutex);
                for(int i = 0;i<SUBCARRIER_COUNT;i++){
                    ifft_input[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING] = default_symbol;
                }
                kiss_fftri(ifft_cfg, ifft_input, ifft_output);
                play_audio_samples_float(handle, ifft_output, FFT_SIZE);
            }
        }
        memset(ifft_input, 0, sizeof(ifft_input));
        pthread_mutex_unlock(&audio_send_queue_mutex);
        for(int i = 0;i<SUBCARRIER_COUNT;i++){
            ifft_input[FIRST_SUBCARRIER + i*SUBCARRIER_SPACING] = default_symbol;
        }
        kiss_fftri(ifft_cfg, ifft_input, ifft_output);
        play_audio_samples_float(handle, ifft_output, FFT_SIZE);
    }

    return NULL;
}

void* serial_input_loop(void* args){
    printf("serial input loop started\n");

    char recive_buffer[1024];
    while(true){
        int n = read(serial_fd, recive_buffer, sizeof(recive_buffer));
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

void cleanup(int sig){
    for(int i = 0;i<4;i++){
        char cmd[128];
        sprintf(cmd,"pactl unload-module %i", audio_devs[i]);
        system(cmd);
    }
    printf("deleted audio devices\n");
    exit(0);
}

int run_command_with_int_output(char* cmd){
    char buffer[128];
    FILE *fp;

    fp = popen(cmd, "r");
    if (fp == NULL) {
        perror("popen failed");
        return 1;
    }

    fgets(buffer, sizeof(buffer), fp);

    pclose(fp);

    return atoi(buffer);
}

int main() {
    printf("configured physical layer speed: %f bps\n", SAMPLE_RATE/FFT_SIZE*SUBCARRIER_COUNT*log2(SUBCARRIER_SYMBOLS_COUNT)/(FFT_SIZE+CYCLIC_PREFIX)*FFT_SIZE);

    //generate symbols
    for(int i = 0;i<SUBCARRIER_SYMBOLS_COUNT;i++){
        kiss_fft_cpx symbol = {(1.0/(SUBCARRIER_SYMBOLS_COUNT-1))*i,0};
        subcarrier_symbols[i] = symbol;
    }

    pthread_mutex_init(&serial_send_queue_mutex, NULL);
    pthread_mutex_init(&audio_send_queue_mutex, NULL);

    //create serial port
    int serial_slave_fd;
    char serial_name[256];
    if (openpty(&serial_fd, &serial_slave_fd, serial_name, NULL, NULL) == -1) {
        perror("openpty failed");
        exit(1);
    }
    printf("created serial port: %s\n", serial_name);

    //create audio devices
    signal(SIGINT, cleanup);
    audio_devs[0] = run_command_with_int_output("pactl load-module module-null-sink sink_name=ReceiverSpeaker sink_properties=device.description=ReceiverSpeaker");
    audio_devs[1] = run_command_with_int_output("pactl load-module module-null-sink sink_name=TransmitterSpeaker sink_properties=device.description=TransmitterSpeaker");
    audio_devs[2] = run_command_with_int_output("pactl load-module module-remap-source source_name=ReceiverMicrophone master=ReceiverSpeaker.monitor source_properties=device.description=ReceiverMicrophone");
    audio_devs[3] = run_command_with_int_output("pactl load-module module-remap-source source_name=TransmitterMicrophone master=TransmitterSpeaker.monitor source_properties=device.description=TransmitterMicrophone");
    printf("created audio devices\n");

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

    sleep_ms(100);

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

    cleanup(0);

    return 0;
}