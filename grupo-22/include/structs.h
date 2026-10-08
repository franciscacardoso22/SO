#ifndef STRUCTS_H
#define STRUCTS_H

#include <sys/time.h> 

#define MAX_CMD_SIZE 512
#define MAX_QUEUE 100

typedef struct {
    int pid;            
    int user_id;        
    char operation;     
    char command[MAX_CMD_SIZE]; 
    int status;         
    
    struct timeval arrival_time; 
} Message;

#endif