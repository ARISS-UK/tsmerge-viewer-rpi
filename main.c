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
#include <signal.h>

static int terminated;

#include "ariss_overlay.h"
#include "backgroundLayer.h"
#include "imageLayer.h"
#include "key.h"
#include "loadpng.h"
#include "bcm_host.h"

#include "bcm_host.h"
#include "ilclient.h"

#include "input_buffer.h"
rxBuffer_t rxBuffer;
rxBuffer_t rxTcpBuffer;

#include "ts/ts.h"

#define TCP_RX_WAIT	4000

#define VIDEO_PID	256

#define SCREEN_RESOLUTION_HEIGHT  768
#define SCREEN_RESOLUTION_WIDTH   1024

static FILE *input_file;
static int input_socket;

enum input_source_t {SOURCE_TCP, SOURCE_FILE, SOURCE_NONE};
static enum input_source_t input_source = SOURCE_NONE;

static uint64_t data_received = 0;

static inline OMX_TICKS ToOMXTime(int64_t pts)
{
  OMX_TICKS ticks;
  ticks.nLowPart = pts;
  ticks.nHighPart = pts >> 32;
  return ticks;
}
static inline int64_t FromOMXTime(OMX_TICKS ticks)
{
  int64_t pts = ticks.nLowPart | ((uint64_t)(ticks.nHighPart) << 32);
  return pts;
}

uint64_t timestamp_ms(void)
{
    struct timespec tp;

    if(clock_gettime(CLOCK_REALTIME, &tp) != 0)
    {
        return(0);
    }

    return((uint64_t) tp.tv_sec * 1000 + tp.tv_nsec / 1000000);
}

static uint64_t timestamp(void) {
  uint64_t _ts = 0;
  
  struct timespec spec;
  clock_gettime(CLOCK_REALTIME, &spec);
  _ts = (uint64_t) spec.tv_sec;

  return _ts;
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

#define OVERLAY_DISPLAY_NUM	2
#define OVERLAY_LAYER_NUM	1
#define OVERLAY_LAYER_X		(1*(SCREEN_RESOLUTION_WIDTH-450)/45)
#define OVERLAY_LAYER_Y		(7*(SCREEN_RESOLUTION_HEIGHT-106)/8)

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

    printf("Video in buffer. Preparing overlay..\n");

    char tmp_overlay_filename[] = "/tmp/ariss-video-overlay_XXXXXX";
    int tmp_overlay_file = mkstemp(tmp_overlay_filename);
    if(tmp_overlay_file < 0)
    {
        printf("Error opening temporary file for overlay!\n");
        break;
    }
    write(tmp_overlay_file, ariss_overlay_png, ariss_overlay_png_len);
    close(tmp_overlay_file);

    DISPMANX_MODEINFO_T overlay_info;
    IMAGE_LAYER_T overlay_imageLayer;
    DISPMANX_DISPLAY_HANDLE_T overlay_display = vc_dispmanx_display_open(OVERLAY_DISPLAY_NUM);
    if(overlay_display == 0)
    {
      fprintf(stderr,"Error opening dispmanx display\n");
    }
    if(vc_dispmanx_display_get_info(overlay_display, &overlay_info) != 0)
    {
      fprintf(stderr, "Error loading dispmanx display info.\n");
    }
    if (loadPng(&(overlay_imageLayer.image), tmp_overlay_filename) == false)
    {
      fprintf(stderr, "Error loading overlay png: %s\n", tmp_overlay_filename);
    }
    createResourceImageLayer(&overlay_imageLayer, OVERLAY_LAYER_NUM);
    DISPMANX_UPDATE_HANDLE_T overlay_update = vc_dispmanx_update_start(0);
    if(overlay_update == 0)
    {
      fprintf(stderr, "Error creating overlay update handle\n");
    }
    addElementImageLayerOffset(&overlay_imageLayer, OVERLAY_LAYER_X, OVERLAY_LAYER_Y, overlay_display, overlay_update);
    if(vc_dispmanx_update_submit_sync(overlay_update) != 0)
    {
      fprintf(stderr, "Error updating overlay image with offsets\n");
    }

    printf("Letting video pre-buffer..\n");
    usleep(750*1000);
    
    printf("Starting Video player..\n");
    video_play();
    printf("Video player stopped.\n");

    printf("Stopping video playback.\n");
    destroyImageLayer(&overlay_imageLayer);
    if(vc_dispmanx_display_close(overlay_display) != 0)
    {
      fprintf(stderr, "Error closing dispmanx display\n");
    }
    unlink(tmp_overlay_filename);
    printf("Overlay stopped. Waiting for data..");
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
  format.xFramerate = 29.97 * (1<<16);

  /* Set fullscreen 1080x1920, forced aspect ratio */
  OMX_ERRORTYPE omx_err;
  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  memset(&configDisplay, 0, sizeof(configDisplay));
  configDisplay.nSize = sizeof(configDisplay);
  configDisplay.nVersion.s.nVersionMajor = OMX_VERSION_MAJOR;
  configDisplay.nVersion.s.nVersionMinor = OMX_VERSION_MINOR;
  configDisplay.nVersion.s.nRevision = OMX_VERSION_REVISION;
  configDisplay.nVersion.s.nStep = OMX_VERSION_STEP;
  configDisplay.nPortIndex = 90; //m_omx_render.GetInputPort();

  configDisplay.set        = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_NOASPECT | OMX_DISPLAY_SET_MODE | OMX_DISPLAY_SET_SRC_RECT | OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_PIXEL);
  configDisplay.noaspect   = OMX_FALSE; //OMX_TRUE;
  configDisplay.mode       = OMX_DISPLAY_MODE_LETTERBOX;

  configDisplay.src_rect.x_offset   = (int)(0+0.5f);
  configDisplay.src_rect.y_offset   = (int)((SCREEN_RESOLUTION_HEIGHT-576)/2+0.5f);
  configDisplay.src_rect.width      = (int)(SCREEN_RESOLUTION_WIDTH+0.5f);
  configDisplay.src_rect.height     = (int)(576+0.5f);

  configDisplay.fullscreen = OMX_TRUE;
  configDisplay.pixel_x = 0;
  configDisplay.pixel_y = 0;
  configDisplay.layer = 1;
  omx_err = OMX_SetConfig(ILC_GET_HANDLE(video_render), OMX_IndexConfigDisplayRegion, &configDisplay);	
  if (omx_err != OMX_ErrorNone)
  {
    printf("Error setting render display options! omx_err(0x%08x)\n", omx_err);
  }

  OMX_PARAM_BRCMVIDEODECODEERRORCONCEALMENTTYPE concanParam;
  memset(&concanParam, 0, sizeof(concanParam));
  concanParam.nSize = sizeof(concanParam);
  concanParam.nVersion.s.nVersionMajor = OMX_VERSION_MAJOR;
  concanParam.nVersion.s.nVersionMinor = OMX_VERSION_MINOR;
  concanParam.nVersion.s.nRevision = OMX_VERSION_REVISION;
  concanParam.nVersion.s.nStep = OMX_VERSION_STEP;
  
  concanParam.bStartWithValidFrame = OMX_TRUE;

  omx_err = OMX_SetConfig(ILC_GET_HANDLE(video_decode), OMX_IndexParamBrcmVideoDecodeErrorConcealment, &concanParam);
  if (omx_err != OMX_ErrorNone)
  {
    printf("Error setting decode codec_error_concealment options! omx_err(0x%08x)\n", omx_err);
  }

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
         buf->nFilledLen = ts_read(buf->pBuffer, buf->nAllocLen);
         
         if(buf->nFilledLen == 0)
            break;
         
         /*
         int i;
         int64_t pts = 0;
         for(i=0;i<buf->nFilledLen;i++)
         {
           if(buf->pBuffer[0] == 0x00)
           {
             if(buf->pBuffer[1] == 0x00)
             {
               if(buf->pBuffer[2] == 0x01)
               {
                 //printf("Possible PES Start, Stream ID: 0x%02x, Length:0x%02x%02x\n"
                 //  ,buf->pBuffer[3]
                 //  ,buf->pBuffer[4],buf->pBuffer[5]
                 //);
                 //printf(" - PTS/DTS: %02x, Extra length: %02x\n"
                 //  ,(buf->pBuffer[7] & 0xc0) >> 6
                 //  ,buf->pBuffer[8]
                 //);
                 //printf(" - Extra ID: 0x%02x\n"
                 //  ,(buf->pBuffer[9] & 0xF0) >> 4
                 //);
                 pts |= (buf->pBuffer[9] & 0x0E) << 30;
                 pts |= (buf->pBuffer[10] & 0xFF) << 22;
                 pts |= (buf->pBuffer[11] & 0xFE) << 15;
                 pts |= (buf->pBuffer[12] & 0xFF) << 7;
                 pts |= (buf->pBuffer[11] & 0xFE) >> 1;

                 //printf("PTS: 0x%lld\n",pts);
               }
             }
           }
         }
         */

         buf->nOffset = 0;

         if(port_settings_changed == 0 &&
            ((buf->nFilledLen > 0 && ilclient_remove_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) ||
            (buf->nFilledLen == 0 && ilclient_wait_for_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1,
            ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0)))
         {
            port_settings_changed = 1;

            if(ilclient_setup_tunnel(tunnel, 0, 0) != 0)
            {
               status = -7;
               break;
            }

            ilclient_change_component_state(video_scheduler, OMX_StateExecuting);

            // now setup tunnel to video_
            if(ilclient_setup_tunnel(tunnel+1, 0, 1000) != 0)
            {
               status = -12;
               break;
            }

            ilclient_change_component_state(video_render, OMX_StateExecuting);
         }

         if(first_packet)
         {
            buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
            first_packet = 0;
         }
         else
         {
            buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
            //buf->nFlags = 0; // PTS
            //buf->nTimeStamp = ToOMXTime(pts);
         }

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
		"Usage: ariss-video [options]\n"
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
	uint8_t buffer[16384];
	uint32_t length;

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
      //printf("Pushing to tcp buffer (%d bytes)\n", length);
			rxBufferPush(&rxTcpBuffer, buffer, length);
			data_received = timestamp();
		}
	}
}

pthread_t tcp_buffer_thread;
void* tcp_buffer_loop(void *arg)
{
	(void) arg;
	int status;
	uint8_t buffer[16384];
	uint32_t length;
	uint8_t video_buffer[16384];
  ts_header_t ts_header;

    uint64_t last_one = 0;

	while(1)
	{
		length = rxBufferWaitTSPop(&rxTcpBuffer, buffer);

		if(length != TS_PACKET_SIZE)
		{
			printf("Incoming packet invalid size, expected %d bytes, got %d\n", TS_PACKET_SIZE, length);
			continue;
		}

		/* Feed in the packet(s) */
		if((status = ts_parse_header(&ts_header, buffer)) != TS_OK)
		{
			printf("Failed to parse TS Header! (Error: %d)\n", status);
			continue;
		}

		if((ts_header.pid == VIDEO_PID) && (ts_header.payload_flag > 0))
		{
			memcpy(video_buffer,&buffer[ts_header.payload_offset], (TS_PACKET_SIZE - ts_header.payload_offset));
			if((timestamp_ms()-last_one)>10)
			  //printf("Pushing to video buffer (%d bytes), gap: %lu ms\n", (TS_PACKET_SIZE - ts_header.payload_offset), (timestamp_ms()-last_one));
                        last_one = timestamp_ms();



			rxBufferPush(&rxBuffer, video_buffer, (TS_PACKET_SIZE - ts_header.payload_offset));
		}
	}
}

void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        terminated = 1;
    }
}

int main (int argc, char **argv)
{
	int opt,c;
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

	terminated = 0;
    if(signal(SIGINT, signal_handler) == SIG_ERR
     || signal(SIGTERM, signal_handler) == SIG_ERR)
    {
        printf("Error setting up signal handler!\n");
        return 1;
    }

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
			rxBufferInit(&rxTcpBuffer);

			tcp_rx_params.hostname = hostname;
			tcp_rx_params.port = port;
			tcp_rx_params.ai_family = ai_family;
    		pthread_create(&tcp_rx_thread, NULL, &tcp_rx_loop, NULL);

    		pthread_create(&tcp_buffer_thread, NULL, &tcp_buffer_loop, NULL);

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

   	while(!terminated)
        {
          //printf("Head - Tail: %d\n",rxBufferHead(&rxBuffer) - rxBufferTail(&rxBuffer));
          usleep(50*1000);
        }

   	switch(input_source)
	{
		case SOURCE_FILE:
			fclose(input_file);
			break;

		case SOURCE_TCP:
			close(input_socket);
			break;
		case SOURCE_NONE:
			break;
	}

	return 0;
}
