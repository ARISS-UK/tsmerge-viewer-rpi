#ifndef __MERGER_RX_BUFFER_H
#define __MERGER_RX_BUFFER_H

#include <pthread.h>

#define RX_BUFFER_LENGTH    1048576

typedef struct
{
    /* Buffer Access Lock */
    pthread_mutex_t Mutex;
    /* New Data Signal */
    pthread_cond_t Signal;
    /* Head and Tail Indexes */
    uint32_t Head, Tail;
    /* Data Loss Counter */
    uint32_t Loss;
    /* Data */
    uint8_t Buffer[RX_BUFFER_LENGTH];
} rxBuffer_t;

/** Common functions **/
void rxBufferInit(void *buffer_void_ptr);
uint8_t rxBufferNotEmpty(void *buffer_void_ptr);
uint16_t rxBufferHead(void *buffer_void_ptr);
uint16_t rxBufferTail(void *buffer_void_ptr);
uint32_t rxBufferLoss(void *buffer_void_ptr);
void rxBufferPush(void *buffer_void_ptr, uint8_t *data_p, uint32_t length);
uint32_t rxBufferPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length);
uint32_t rxBufferWaitPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length);
uint32_t rxBufferWaitTSPop(void *buffer_void_ptr, uint8_t *bufferPtr);
uint32_t rxBufferTimedWaitPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length, uint32_t milliseconds);

#endif
