CONTIKI_PROJECT = ugrid_controller
all: $(CONTIKI_PROJECT)

# include CBOR
MODULES += os/lib/cbor

# include CoAP module
MODULES += os/net/app-layer/coap

# include CoAP resource
MODULES_REL += ./resources

# include emlearn for ML inference
TARGET_LIBFILES += -lm
MODULES_REL += ../.venv/lib/python3.9/site-packages/emlearn
INC += ../.venv/lib/python3.9/site-packages/emlearn


CONTIKI = ../../..
include $(CONTIKI)/Makefile.include

