#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pty.h>
#include <string.h>
#include <pthread.h>


void* receive_loop(void* args){
    int serial_fd = *(int*)(args);
    char recive_buffer[1024];
    while(1){
        int n = read(serial_fd, recive_buffer, sizeof(recive_buffer) - 1);
        if (n > 0) {
            recive_buffer[n] = '\0';
            printf("%s", recive_buffer);
        }
    }
    return NULL;
}

int main() {
    int serial_fd;
    int slave_fd;
    char slave_name[256];
    char send_buffer[1024];
    pthread_t receive_thread;
    
    if (openpty(&serial_fd, &slave_fd, slave_name, NULL, NULL) == -1) {
        perror("openpty failed");
        exit(1);
    }
    
    printf("virtual serial port: %s\n", slave_name);
    
    if (pthread_create(&receive_thread, NULL, receive_loop, &serial_fd) != 0) {
        fprintf(stderr, "Error creating rerceive thread\n");
        return 1;
    }
        
    while (1) {
        if (fgets(send_buffer, sizeof(send_buffer), stdin) == NULL) break;
        write(serial_fd, send_buffer, strlen(send_buffer));
    }



    if (pthread_join(receive_thread, NULL) != 0) {
        fprintf(stderr, "Error joining thread\n");
        return 1;
    }
    
    close(serial_fd);
    return 0;
}