#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "input_buffer.h"
#include "ts/ts.h"

void rxBufferInit(void *buffer_void_ptr)
{
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    pthread_mutex_init(&buf->Mutex, NULL);
    pthread_cond_init(&buf->Signal, NULL);
    
    pthread_mutex_lock(&buf->Mutex);
    buf->Head = 0;
    buf->Tail = 0;
    buf->Loss = 0;
    pthread_mutex_unlock(&buf->Mutex);
}

uint8_t rxBufferNotEmpty(void *buffer_void_ptr)
{
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;

    uint8_t result;
    
    pthread_mutex_lock(&buf->Mutex);
    result = (buf->Head!=buf->Tail);
    pthread_mutex_unlock(&buf->Mutex);
    
    return result;
}

uint16_t rxBufferHead(void *buffer_void_ptr)
{
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    uint16_t result;
    
    pthread_mutex_lock(&buf->Mutex);
    result = buf->Head;
    pthread_mutex_unlock(&buf->Mutex);
    
    return result;
}

uint16_t rxBufferTail(void *buffer_void_ptr)
{
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    uint16_t result;
    
    pthread_mutex_lock(&buf->Mutex);
    result = buf->Tail;
    pthread_mutex_unlock(&buf->Mutex);
    
    return result;
}

uint32_t rxBufferLoss(void *buffer_void_ptr)
{
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    uint32_t result;
    
    pthread_mutex_lock(&buf->Mutex);
    result = buf->Loss;
    pthread_mutex_unlock(&buf->Mutex);
    
    return result;
}

/* Lossy when buffer is full */
void rxBufferPush(void *buffer_void_ptr, uint8_t *data_p, uint32_t length)
{
    uint32_t i = 0;
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    pthread_mutex_lock(&buf->Mutex);

    while(i < length)
    {
        if(buf->Head == (buf->Tail-1))
        {
            buf->Loss += (length - i);
            break;
        }

        if(buf->Head==(RX_BUFFER_LENGTH-1))
            buf->Head=0;
        else
            buf->Head++;

        buf->Buffer[buf->Head] = data_p[i];
        i++;
    }

    pthread_mutex_unlock(&buf->Mutex);

    pthread_cond_signal(&buf->Signal);
}

uint32_t rxBufferPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length)
{
    uint32_t i = 0;
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    pthread_mutex_lock(&buf->Mutex);

    while(i<buffer_length)
    {
        if(buf->Head==buf->Tail)
        {
            break;
        }

        if(buf->Tail==(RX_BUFFER_LENGTH-1))
            buf->Tail=0;
        else
            buf->Tail++;

        bufferPtr[i] = buf->Buffer[buf->Tail];
        i++;
    }

    pthread_mutex_unlock(&buf->Mutex);

    return i;
}

uint32_t rxBufferWaitPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length)
{
    uint32_t i = 0;
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    pthread_mutex_lock(&buf->Mutex);
    
    while(buf->Head==buf->Tail) /* If buffer is empty */
    {
        /* Mutex is atomically unlocked on beginning waiting for signal */
        pthread_cond_wait(&buf->Signal, &buf->Mutex);
        /* and locked again on resumption */
    }
    
    while(i<buffer_length)
    {
        if(buf->Head==buf->Tail)
        {
            break;
        }

        if(buf->Tail==(RX_BUFFER_LENGTH-1))
            buf->Tail=0;
        else
            buf->Tail++;

        bufferPtr[i] = buf->Buffer[buf->Tail];
        i++;
    }
    
    pthread_mutex_unlock(&buf->Mutex);

    return i;
}

uint32_t rxBufferWaitTSPop(void *buffer_void_ptr, uint8_t *bufferPtr)
{
    uint32_t i = 0;
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;
    
    pthread_mutex_lock(&buf->Mutex);
    
    /* Wait for data in buffer */
    while(buf->Head==buf->Tail || buf->Buffer[buf->Tail] != TS_HEADER_SYNC)
    {
        if(buf->Head==buf->Tail)
        {
            /* Mutex is atomically unlocked on beginning waiting for signal */
            pthread_cond_wait(&buf->Signal, &buf->Mutex);
            /* and locked again on resumption */
        }

        /* Consume data from buffer until it's empty or we find TS_HEADER_SYNC byte */
        while(buf->Head!=buf->Tail)
        {
            if(buf->Tail==(RX_BUFFER_LENGTH-1))
                buf->Tail=0;
            else
                buf->Tail++;

            if(buf->Buffer[buf->Tail] == TS_HEADER_SYNC)
            {
                break;
            }
        }
    }

    /* Wait for TS_PACKET_SIZE bytes following the sync bytes */
    while(((buf->Head > buf->Tail) && (buf->Head < buf->Tail + TS_PACKET_SIZE))
        || ((buf->Head < buf->Tail) && ((int)buf->Head < (int)buf->Tail + TS_PACKET_SIZE - RX_BUFFER_LENGTH)))
    {
        /* Mutex is atomically unlocked on beginning waiting for signal */
        pthread_cond_wait(&buf->Signal, &buf->Mutex);
        /* and locked again on resumption */
    }
    
    i = 0;
    bufferPtr[i] = buf->Buffer[buf->Tail];
    i++;

    while(i < TS_PACKET_SIZE)
    {
        if(buf->Head==buf->Tail)
        {
            break;
        }

        if(buf->Tail==(RX_BUFFER_LENGTH-1))
            buf->Tail=0;
        else
            buf->Tail++;

        bufferPtr[i] = buf->Buffer[buf->Tail];
        i++;
    }
    
    pthread_mutex_unlock(&buf->Mutex);

    return i;
}

uint32_t rxBufferTimedWaitPop(void *buffer_void_ptr, uint8_t *bufferPtr, uint32_t buffer_length, uint32_t milliseconds)
{
    uint32_t i = 0;
    rxBuffer_t *buf;
    buf = (rxBuffer_t *)buffer_void_ptr;

    struct timeval tv;
    struct timespec ts;

    gettimeofday(&tv, NULL);
    ts.tv_sec = time(NULL) + milliseconds / 1000;
    ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (milliseconds % 1000);
    ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
    ts.tv_nsec %= (1000 * 1000 * 1000);

    pthread_mutex_lock(&buf->Mutex);

    if(buf->Head==buf->Tail) /* If buffer is empty */
    {
        
        /* Mutex is atomically unlocked on beginning waiting for signal */
        pthread_cond_timedwait(&buf->Signal, &buf->Mutex, &ts);
        /* and locked again on resumption */
    }

    while(i<buffer_length)
    {
        if(buf->Head==buf->Tail)
        {
            break;
        }

        if(buf->Tail==(RX_BUFFER_LENGTH-1))
            buf->Tail=0;
        else
            buf->Tail++;

        bufferPtr[i] = buf->Buffer[buf->Tail];
        i++;
    }
    
    pthread_mutex_unlock(&buf->Mutex);

    return i;
}