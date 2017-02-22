CC = gcc

BIN = ariss-video

SERVICE = ariss-video

OBJS=main.o input_buffer.o ts/ts.o
LDFLAGS+=-lilclient

CFLAGS+=-g -gdwarf-3 -Og -DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -Wall -g -DHAVE_LIBOPENMAX=2 -DOMX -DOMX_SKIP64BIT -ftree-vectorize -pipe -DUSE_EXTERNAL_OMX -DHAVE_LIBBCM_HOST -DUSE_EXTERNAL_LIBBCM_HOST -DUSE_VCHIQ_ARM -Wno-psabi

LDFLAGS+=-L$(SDKSTAGE)/opt/vc/lib/ -lGLESv2 -lEGL -lopenmaxil -lbcm_host -lvcos -lvchiq_arm -lpthread -lrt -lm -L$(SDKSTAGE)/opt/vc/src/hello_pi/libs/ilclient -L$(SDKSTAGE)/opt/vc/src/hello_pi/libs/vgfont

INCLUDES+=-I/opt/vc/include -I/opt/vc/include/interface/vmcs_host/linux -I/opt/vc/include/interface/vcos/pthreads -I$(SDKSTAGE)/opt/vc/include/ -I$(SDKSTAGE)/opt/vc/include/interface/vcos/pthreads -I$(SDKSTAGE)/opt/vc/include/interface/vmcs_host/linux -I./ -I$(SDKSTAGE)/opt/vc/src/hello_pi/libs/ilclient -I$(SDKSTAGE)/opt/vc/src/hello_pi/libs/vgfont


OBJS+= raspidmx/common/backgroundLayer.o raspidmx/common/imageLayer.o raspidmx/common/loadpng.o raspidmx/common/image.o raspidmx/common/key.o

CFLAGS+=-Iraspidmx/common $(shell libpng-config --cflags)

LDFLAGS+=-L/opt/vc/lib/ -lbcm_host -lm $(shell libpng-config --ldflags)

INCLUDES+=-I/opt/vc/include/ -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux

all: $(OBJS)
	$(CC) -g -gdwarf-3 -Og -o $(BIN) -Wl,--whole-archive $(OBJS) $(LDFLAGS) -Wl,--no-whole-archive -rdynamic

%.o: %.c
	xxd -i ariss_overlay.png > ariss_overlay.h
	@rm -f $@ 
	$(CC) $(CFLAGS) $(INCLUDES) -g -c $< -o $@ -Wno-deprecated-declarations

%.o: %.cpp
	@rm -f $@ 
	$(CXX) $(CFLAGS) $(INCLUDES) -g -c $< -o $@ -Wno-deprecated-declarations

%.a: $(OBJS)
	$(AR) r $@ $^

clean:
	@rm -f $(OBJS) $(BIN)

install:
	@cp -fv $(BIN) /usr/bin/$(BIN)
	@cp -fv $(SERVICE).service /etc/systemd/system/$(SERVICE).service
	@systemctl daemon-reload
	systemctl enable $(SERVICE)
	systemctl start $(SERVICE)

