/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Video deocode demo using OpenMAX IL though the ilcient helper library

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>

#include "bcm_host.h"
#include "ilclient.h"

#include "input_buffer.h"
rxBuffer_t rxBuffer;

#include "ts/ts.h"

#define TCP_RX_WAIT	4000

#define VIDEO_PID	256

static FILE *input_file;
static int input_socket;

enum input_source_t {SOURCE_TCP, SOURCE_FILE, SOURCE_NONE};
static enum input_source_t input_source = SOURCE_NONE;

static uint64_t data_received = 0;

static uint64_t timestamp(void) {
  uint64_t _ts = 0;
  
  struct timespec spec;
  clock_gettime(CLOCK_REALTIME, &spec);
  _ts = (uint64_t) spec.tv_sec;

  return _ts;
}

static void sleep_ms(uint32_t _duration)
{
    struct timespec req, rem;
    req.tv_sec = _duration / 1000;
    req.tv_nsec = (_duration - (req.tv_sec*1000))*1000*1000;
    
    while(nanosleep(&req, &rem) == EINTR)
    {
        /* Interrupted by signal, shallow copy remaining time into request, and resume */
        req = rem;
    }
}

static unsigned int ts_read(unsigned char* destination, unsigned int length)
{
	switch(input_source)
	{
		case SOURCE_FILE:
			return fread(destination, 1, length, input_file);

		case SOURCE_TCP:
			return rxBufferTimedWaitPop(&rxBuffer, destination, length, TCP_RX_WAIT);
		case SOURCE_NONE:
		default:
			return 0;
	}
}

//static int video_decode_test(void)
pthread_t video_thread;
void video_play(void);
void* video_loop(void *arg)
{
	(void) arg;
	uint32_t canary_length = 1;
	uint8_t canary_buffer[1];

	while(1)
	{
	    while(ts_read(canary_buffer,canary_length) == 0) {};

		printf("Starting video playback..\n");
		video_play();
	}
}
void video_play(void)
{
   OMX_VIDEO_PARAM_PORTFORMATTYPE format;
   OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
   COMPONENT_T *video_decode = NULL, *video_scheduler = NULL, *video_render = NULL, *clock = NULL;
   COMPONENT_T *list[5];
   TUNNEL_T tunnel[4];
   ILCLIENT_T *client;
   int status = 0;
   unsigned int data_len = 0;

   memset(list, 0, sizeof(list));
   memset(tunnel, 0, sizeof(tunnel));

   if((client = ilclient_init()) == NULL)
   {
   		printf("Error: Null video client!\n");
      return;
   }

   if(OMX_Init() != OMX_ErrorNone)
   {
      ilclient_destroy(client);
   		printf("Error: OMX Init failed!\n");
      return;
   }

   // create video_decode
   if(ilclient_create_component(client, &video_decode, "video_decode", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0)
      status = -14;
   list[0] = video_decode;

   // create video_render
   if(status == 0 && ilclient_create_component(client, &video_render, "video_render", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[1] = video_render;

   // create clock
   if(status == 0 && ilclient_create_component(client, &clock, "clock", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[2] = clock;

   memset(&cstate, 0, sizeof(cstate));
   cstate.nSize = sizeof(cstate);
   cstate.nVersion.nVersion = OMX_VERSION;
   cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
   cstate.nWaitMask = 1;
   if(clock != NULL && OMX_SetParameter(ILC_GET_HANDLE(clock), OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone)
      status = -13;

   // create video_scheduler
   if(status == 0 && ilclient_create_component(client, &video_scheduler, "video_scheduler", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[3] = video_scheduler;

   set_tunnel(tunnel, video_decode, 131, video_scheduler, 10);
   set_tunnel(tunnel+1, video_scheduler, 11, video_render, 90);
   set_tunnel(tunnel+2, clock, 80, video_scheduler, 12);

   // setup clock tunnel first
   if(status == 0 && ilclient_setup_tunnel(tunnel+2, 0, 0) != 0)
      status = -15;
   else
      ilclient_change_component_state(clock, OMX_StateExecuting);

   if(status == 0)
      ilclient_change_component_state(video_decode, OMX_StateIdle);

   memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
   format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
   format.nVersion.nVersion = OMX_VERSION;
   format.nPortIndex = 130;
   //format.eCompressionFormat = OMX_VIDEO_CodingAVC;
   //format.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
   format.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
   //format.eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
	

   if(status == 0 &&
      OMX_SetParameter(ILC_GET_HANDLE(video_decode), OMX_IndexParamVideoPortFormat, &format) == OMX_ErrorNone &&
      ilclient_enable_port_buffers(video_decode, 130, NULL, NULL, NULL) == 0)
   {
      OMX_BUFFERHEADERTYPE *buf;
      int port_settings_changed = 0;
      int first_packet = 1;

      ilclient_change_component_state(video_decode, OMX_StateExecuting);

      while((buf = ilclient_get_input_buffer(video_decode, 130, 1)) != NULL)
      {
         // feed data and wait until we get port settings changed
         unsigned char *dest = buf->pBuffer;

         //data_len += fread(dest, 1, buf->nAllocLen-data_len, in);
         uint32_t data_read = ts_read(dest, 500); //buf->nAllocLen-data_len);

         if(data_read == 0)
         {
         	break;
         }

         data_len += data_read;

         if(port_settings_changed == 0 &&
            ((data_len > 0 && ilclient_remove_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) ||
             (data_len == 0 && ilclient_wait_for_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1,
                                                       ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0)))
         {
            port_settings_changed = 1;

            if(ilclient_setup_tunnel(tunnel, 0, 0) != 0)
            {
               status = -7;
               break;
            }

            ilclient_change_component_state(video_scheduler, OMX_StateExecuting);

            // now setup tunnel to video_render
            if(ilclient_setup_tunnel(tunnel+1, 0, 1000) != 0)
            {
               status = -12;
               break;
            }

            ilclient_change_component_state(video_render, OMX_StateExecuting);
         }
         if(!data_len)
            break;

         buf->nFilledLen = data_len;
         data_len = 0;

         buf->nOffset = 0;
         if(first_packet)
         {
            buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
            first_packet = 0;
         }
         else
            buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;

         if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
         {
            status = -6;
            break;
         }
      }

      buf->nFilledLen = 0;
      buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;

      if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
         status = -20;

      // wait for EOS from render
      ilclient_wait_for_event(video_render, OMX_EventBufferFlag, 90, 0, OMX_BUFFERFLAG_EOS, 0,
                              ILCLIENT_BUFFER_FLAG_EOS, -1);

      // need to flush the renderer to allow video_decode to disable its input port
      ilclient_flush_tunnels(tunnel, 0);

   }

   ilclient_disable_tunnel(tunnel);
   ilclient_disable_tunnel(tunnel+1);
   ilclient_disable_tunnel(tunnel+2);
   ilclient_disable_port_buffers(video_decode, 130, NULL, NULL, NULL);
   ilclient_teardown_tunnels(tunnel);

   ilclient_state_transition(list, OMX_StateIdle);
   ilclient_state_transition(list, OMX_StateLoaded);

   ilclient_cleanup_components(list);

   OMX_Deinit();

   ilclient_destroy(client);
   return;
}

static int ts_file_open(char* filename)
{
	if((input_file = fopen(filename, "rb")) == NULL)
      	return -1;
  	else
  		return 0;
}

static int ts_tcp_open(char *hostname, char *port, int ai_family)
{
	int r;
	struct addrinfo hints;
	struct addrinfo *re, *rp;
	char s[INET6_ADDRSTRLEN];
	
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = ai_family;
	hints.ai_socktype = SOCK_STREAM;
	
	r = getaddrinfo(hostname, port, &hints, &re);
	if(r != 0)
	{
		printf("Error resolving hostname: %s\n", gai_strerror(r));
		return(-1);
	}
	
	/* Try IPv6 first */
	for(input_socket = -1, rp = re; input_socket == -1 && rp != NULL; rp = rp->ai_next)
	{
		if(rp->ai_addr->sa_family != AF_INET6) continue;
		
		inet_ntop(AF_INET6, &(((struct sockaddr_in6 *) rp->ai_addr)->sin6_addr), s, INET6_ADDRSTRLEN);
		//printf("Connecting to [%s]:%d\n", s, ntohs((((struct sockaddr_in6 *) rp->ai_addr)->sin6_port)));
		
		input_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(input_socket == -1)
		{
			//perror("socket");
			continue;
		}
		
		if(connect(input_socket, rp->ai_addr, rp->ai_addrlen) == -1)
		{
			//perror("connect");
			close(input_socket);
			input_socket = -1;
		}
	}
	
	/* Try IPv4 next */
	for(rp = re; input_socket == -1 && rp != NULL; rp = rp->ai_next)
	{
		if(rp->ai_addr->sa_family != AF_INET) continue;
		
		inet_ntop(AF_INET, &(((struct sockaddr_in *) rp->ai_addr)->sin_addr), s, INET6_ADDRSTRLEN);
		//printf("Connecting to %s:%d\n", s, ntohs((((struct sockaddr_in *) rp->ai_addr)->sin_port)));
		
		input_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(input_socket == -1)
		{
			//perror("socket");
			continue;
		}
		
		if(connect(input_socket, rp->ai_addr, rp->ai_addrlen) == -1)
		{
			//perror("connect");
			close(input_socket);
			input_socket = -1;
		}
	}
	
	freeaddrinfo(re);

	int value = 1;
	if (setsockopt(input_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&value, sizeof(int)) < 0)
      return -2;

	if(input_socket < 0)
		return -1;
	else
  		return 0;
}

static void _print_usage(void)
{
	printf(
		"\n"
		"Usage: tsplay [options]\n"
		"\n"
		"  -f, --file <name>      Set filename to read from. (conflicts with -h)\n"
		"  -h, --host <name>      Set the hostname to read data from (conflicts with -f)\n"
		"  -p, --port <number>    Set the port number to connect to on the host. Default: 5679\n"
		"  -4, --ipv4             Force IPv4 only.\n"
		"  -6, --ipv6             Force IPv6 only.\n"
		"\n"
	);
}

pthread_t tcp_rx_thread;
struct tcp_rx_params_t {
    char *hostname;
    char *port;
    int ai_family;
};
struct tcp_rx_params_t tcp_rx_params;
void* tcp_rx_loop(void *arg)
{
	(void) arg;
	int status;
	int i;
	uint8_t buffer[16384];
	uint32_t length;
	uint8_t video_buffer[16384];
	uint32_t video_length;

	printf("Attempting to connect to TCP Input %s:%s\n",tcp_rx_params.hostname,tcp_rx_params.port);
	while((status = ts_tcp_open(tcp_rx_params.hostname, tcp_rx_params.port, tcp_rx_params.ai_family)) < 0)
	{
		sleep(1);
	}

	printf("TCP Input Connected to %s:%s\n",tcp_rx_params.hostname,tcp_rx_params.port);

	while(1)
	{
		length = recv(input_socket, buffer, 16384, 0);
		if(length==0)
		{
			close(input_socket);

			printf("Attempting to reconnect to TCP Input %s:%s\n",tcp_rx_params.hostname,tcp_rx_params.port);
			while((status = ts_tcp_open(tcp_rx_params.hostname, tcp_rx_params.port, tcp_rx_params.ai_family)) < 0)
			{
				sleep(1);
			}
			printf("TCP Input re-connected to %s:%s\n",tcp_rx_params.hostname,tcp_rx_params.port);
		}
		else if(length<=16384)
		{
			// Extract from TS
			//printf("TCP Packet received, length: %d bytes\n", length);
			
			if(length % 188 != 0)
			{
				//fprintf(stderr, "Incoming packet invalid size, expected a multiple of 188 bytes, got %d\n", length);
				//continue;
			}

			ts_header_t ts_header;
			
			/* Feed in the packet(s) */
			video_length = 0;
			for(i = 0; i < length; i += 188)
			{
				while(buffer[i] != TS_HEADER_SYNC && i < length)
				{
					i++;
				}

				if((status = ts_parse_header(&ts_header, &buffer[i])) != TS_OK)
				{
					//printf("Failed to parse TS Header! (Error: %d)\n", status);
					continue;
				}

				if((ts_header.pid == VIDEO_PID) && (ts_header.payload_flag > 0))
				{
					memcpy(&video_buffer[video_length],&buffer[i+ts_header.payload_offset], 188 - ts_header.payload_offset);
					video_length += (188 - ts_header.payload_offset);

				}
			}
			if(video_length > 0)
			{
				//printf("Pushing to video buffer (%d bytes)\n", video_length);
				rxBufferPush(&rxBuffer, video_buffer, video_length);
			}
			data_received = timestamp();
		}
	}

}

int main (int argc, char **argv)
{
	int opt,c,status;
	char *filename = NULL;
	char *hostname = NULL;
	char *port = "5679";
	int ai_family = AF_INET;

	static const struct option long_options[] = {
		{ "file",        required_argument, 0, 'f' },
		{ "host",        required_argument, 0, 'h' },
		{ "port",        required_argument, 0, 'p' },
		{ "ipv6",        no_argument,       0, '6' },
		{ "ipv4",        no_argument,       0, '4' },
		{ 0,             0,                 0,  0  }
	};

	opterr = 0;
	while((c = getopt_long(argc, argv, "f:h:p:64", long_options, &opt)) != -1)
	{
		switch(c)
		{
		case 'f': /* --filename <name> */
			filename = optarg;
			input_source = SOURCE_FILE;
			break;

		case 'h': /* --host <name> */
			hostname = optarg;
			input_source = SOURCE_TCP;
			break;
		
		case 'p': /* --port <number> */
			port = optarg;
			break;
		
		case '6': /* --ipv6 */
			ai_family = AF_INET6;
			break;
		
		case '4': /* --ipv4 */
			ai_family = AF_INET;
			break;
		
		case '?':
			_print_usage();
			return(0);
		}
	}

	if(filename != NULL && hostname != NULL)
	{
		printf("Error: Only one source permitted!");
		_print_usage();
		return(-1);
	}

	switch(input_source)
	{
		case SOURCE_FILE:
			if(ts_file_open(filename) < 0)
			{
				printf("Error: Opening file input failed!\n");
				return(-1);
			}
			else
			{
				printf("File Input Opened: %s\n",filename);
			}
			break;

		case SOURCE_TCP:
			rxBufferInit(&rxBuffer);
			tcp_rx_params.hostname = hostname;
			tcp_rx_params.port = port;
			tcp_rx_params.ai_family = ai_family;
    		pthread_create(&tcp_rx_thread, NULL, &tcp_rx_loop, NULL);
			break;

		case SOURCE_NONE:
			printf("Error: No source specified!\n");
			_print_usage();
			return(-1);
	}
	

	printf("Initialising videocore..\n");
   	bcm_host_init();

   	printf("Starting video thread..\n");
   	pthread_create(&video_thread, NULL, &video_loop, NULL);

   	while(1) {
   		sleep_ms(1000);
   	};

   	switch(input_source)
	{
		case SOURCE_FILE:
			fclose(input_file);
			printf("File Input closed.\n");
			break;

		case SOURCE_TCP:
			close(input_socket);
			printf("TCP Input closed\n");
			break;
		case SOURCE_NONE:
			break;
	}

	return status;
}