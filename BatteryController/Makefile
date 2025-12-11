CONTIKI_PROJECT = battery_controller
all: $(CONTIKI_PROJECT)

TARGET_LDFLAGS += -Wl,-u,_printf_float

MODULES += os/net/app-layer/coap
MODULES_REL += ../.venv/lib/python3.9/site-packages/emlearn
INC += ../.venv/lib/python3.9/site-packages/emlearn

MODULES += os/services/shell

TARGET_LIBFILES += -lm

PROJECT_SOURCEFILES += eml_net_stub.c

CONTIKI = ../../../
include $(CONTIKI)/Makefile.include


