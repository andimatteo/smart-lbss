CONTIKI_PROJECT = ugrid_controller
all: $(CONTIKI_PROJECT)

TARGET_LDFLAGS += -Wl,-u,_printf_float

MODULES += os/net/app-layer/coap
MODULES_REL += ../.venv/lib/python3.9/site-packages/emlearn
INC += ../.venv/lib/python3.9/site-packages/emlearn

TARGET_LIBFILES += -lm
PROJECT_SOURCEFILES += eml_net_stub.c
#define COAP_OBSERVE_CLIENT            1


CONTIKI = ../../../
include $(CONTIKI)/Makefile.include

