#================================================================
#	 @Author:		dyye
#	 @Created:		08/26/2019 03:46:46
#	
#	 @File Name:	Makefile
#	 @Description:
#	
#================================================================

#CROSS_COMPILE=/opt/hisi-linux/x86-arm/arm-himix200-linux/bin/arm-himix200-linux-
CROSS_COMPILE= 

CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld

INC = -I ../libgoahead/inc \
	  -I ./src 
LIB = -L ./lib \
	  -lgo 

CFLAGS = $(INC) -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Wformat-security \
-Wl,-z,relro,-z,now -Wl,--as-needed -Wl,--no-copy-dt-needed-entries -Wl,-z,noexecstatck -Wl,-z,noexecheap -w -g \
-D_REENTRANT -DPIC  -DME_COM_COMPILER=1 -DME_COM_LIB=1 -DME_COM_MATRIXSSL=0 -DME_COM_MBEDTLS=1 -DME_COM_NANOSSL=0 -DME_COM_OPENSSL=0 -DME_COM_OSDEP=1 -DME_COM_SSL=1 -DME_COM_VXWORKS=0  -DME_DEBUG -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\"

LDFLAGS = $(LIB) -lpthread -lrt -lm -ldl
STDFLAG = 

CSRC = $(wildcard ./src/*.c)
COBJS = $(patsubst %.c, %.o, $(CSRC))

TARGET = goahead

all: $(TARGET)

$(TARGET): $(COBJS) 
	$(CC) -Wl,--gc-sections -o $@ -Wl,--start-group $(COBJS) $(LDFLAGS)  -Wl,--end-group


$(COBJS):%.o:%.c
		 $(CC) -c $(CFLAGS) $(STDFLAG) -o $@ $<


.PHONY: clean distclean

clean:
		-rm -f $(COBJS)

distclean: clean
		-rm -f $(TARGET) $(COBJS)
