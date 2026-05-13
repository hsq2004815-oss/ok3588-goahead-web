#############################################################################
# Makefile                                                                  #
#                                                                           #
# Builds the AiNR 1.0 for ARM lib 					    #
#############################################################################
#
#
# =====================================================================================#
#                                                                                      #
#       Filename:  Makefile                                                            #
#                                                                                      #
#    Description:  iflytek systemctl Makefile for CDModule                          #
#                                                                                      #
#        Version:  1.0                                                                 #
#        Created:  03/09/2011 02:47:32 PM                                              #
#       Revision:  none                                                                #
#       Compiler:  gcc                                                                 #
#                                                                                      #
#         Author:  yongwu@iflytek.Com                                                  #
#        Company:  www.iflytek.com                                                     #
#                                                                                      #
# =====================================================================================#
# =====================================================================================#
# build enviroment include
# =====================================================================================#

#   ----------------------------------------------------------------------------
#   Name of the ARM GCC cross compiler & archiver
#   ----------------------------------------------------------------------------
##ARM_TOOLCHAIN_PREFIX  ?= 
##ARM_TOOLCHAIN_PATH = /home/work/licett/toolchain_newest/toolchain
##ARM_CC := $(ARM_TOOLCHAIN_PATH)/bin/$(ARM_TOOLCHAIN_PREFIX)gcc
##ARM_AR := $(ARM_TOOLCHAIN_PATH)/bin/$(ARM_TOOLCHAIN_PREFIX)ar

HOST_CC := g++
HOST_C	:= gcc
HOST_AR := ar

CURRENT_DIR := $(PWD)

GOAHEADDIR = $(CURRENT_DIR)

# Get any compiler flags from the environment
##CFLAGS = -D__SSE__ -D__SSE2__ -D__SSE3__ -D__SSE4__ -D__SSSE3__ -D__MMX__
CFLAGS = -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Wformat-security \
-Wl,-z,relro,-z,now -Wl,--as-needed -Wl,--no-copy-dt-needed-entries -Wl,-z,noexecstatck -Wl,-z,noexecheap -w -g \
-D_REENTRANT -DPIC  -DME_COM_COMPILER=1 -DME_COM_LIB=1 -DME_COM_MATRIXSSL=0 -DME_COM_MBEDTLS=1 -DME_COM_NANOSSL=0 -DME_COM_OPENSSL=0 -DME_COM_OSDEP=1 -DME_COM_SSL=1 -DME_COM_VXWORKS=0  -DME_DEBUG -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" 
GCC_CFLAGS = $(CFLAGS)
GCC_CPPFLAGS = $(CFLAGS)
#GCC_CPPFLAGS += -std=c++11 -Wall -O3 -g -mavx2 -mavx -mfma -liomp5 -Wnarrowing -fno-builtin -fno-strict-aliasing -pedantic -fPIC -fpermissive -pthread -fstack-protector -ffast-math -ftree-vectorize -msse2 -msse3 -msse4
GCC_CPPFLAGS += -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Wformat-security -Wl,-z,relro,-z,now -Wl,--as-needed -Wl,--no-copy-dt-needed-entries -Wl,-z,noexecstatck -Wl,-z,noexecheap -w -g 
GCC_LDFLAGS = $(LDFLAGS)
GCC_ARFLAGS = rcs

goahead_lib_srcs=\
	$(wildcard $(GOAHEADDIR)/src/mbedtls/*.c) \
	$(wildcard $(GOAHEADDIR)/src/goahead-mbedtls/*.c) \
	$(wildcard $(GOAHEADDIR)/src/src/*.c)

goahead_inc= \
	-I$(GOAHEADDIR)/inc

CINCLUDES += $(goahead_inc)


#   ----------------------------------------------------------------------------
#   List of source files
#   ----------------------------------------------------------------------------

# LIB_SRCS := $(wildcard *.c *.cpp)
LIB_SRCS = 
LIB_SRCS += $(goahead_lib_srcs)
# LIB_OBJS := $(LIB_SRCS:%.c=objects/%.o)
# LIB_OBJS += $(LIB_SRCS:%.cpp=objects/%.o)
# LIB_OBJS += $(LIB_SRCS:%.cc=objects/%.o)
# LIB_OBJS = $(patsubst %.cpp,%.o,$(LIB_SRCS))
# LIB_OBJS += $(patsubst %.cc,%.o,$(LIB_SRCS))
LIB_OBJS += $(patsubst %.c,%.o,$(LIB_SRCS))

#   ----------------------------------------------------------------------------
#   Makefile targets
#   ----------------------------------------------------------------------------
.PHONY : libobjects soobjects libclean all clean

KERNEL_LIB = libgo.a
KERNEL_SO = libgo.so

all: libobjects soobjects
clean: libclean


libobjects: $(LIB_OBJS)
	$(HOST_AR) $(GCC_ARFLAGS) $(KERNEL_LIB) $(LIB_OBJS)

soobjects: $(LIB_OBJS)
	$(HOST_C) -shared -o $(KERNEL_SO) $(LIB_OBJS)

%.o : %.cpp
	$(HOST_CC) -c $(GCC_CPPFLAGS) $(CINCLUDES) -o $@ $<

%.o : %.cc
	$(HOST_CC) -c $(GCC_CPPFLAGS) $(CINCLUDES) -o $@ $<

%.o : %.c 
	$(HOST_C) -c $(GCC_CFLAGS) $(CINCLUDES) -o $@ $<


libclean:
	@rm -Rf $(KERNEL_LIB)
	@rm -Rf objects

clean:
	@rm -Rf $(KERNEL_LIB) $(KERNEL_SO) $(LIB_OBJS)

show:
	@echo PWD:$(PWD)
	@echo MAXENGINEDIR:$(MAXENGINEDIR)
	@echo CINCLUDES:$(CINCLUDES)
	@echo LIB_OBJS:$(LIB_OBJS)
