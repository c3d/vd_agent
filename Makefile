MIQ=make-it-quick/

VARIANTS=$(VARIANTS_$(OS_NAME))
VARIANTS_linux=sbin bin
VARIANTS_macosx=bin

PRODUCTS=$(PRODUCTS_$(VARIANT))

PRODUCTS_bin=spice-vdagent.exe
PRODUCTS_sbin=spice-vdagentd.exe

PREFIX_BIN=$(PREFIX)$(VARIANT)/

SOURCES=					\
	src/udscs.c				\
	$(SOURCES_$(VARIANT))

SOURCES_bin =					\
	src/vdagent/audio.c			\
	src/vdagent/clipboard.c			\
	src/vdagent/file-xfers.c		\
	src/vdagent/x11-randr.c			\
	src/vdagent/x11.c			\
	src/vdagent/vdagent.c			\

SOURCES_sbin=					\
	src/vdagentd/vdagentd.c			\
	src/vdagentd/xorg-conf.c		\
	src/vdagentd/virtio-port.c		\
	src/vdagentd/uinput.c

ifdef HAVE_CONSOLE_KIT
SOURCES_sbin += src/vdagentd/console-kit.c
else
ifdef HAVE_LIBSYSTEMD_LOGIN
SOURCES_sbin += src/vdagentd/systemd-login.c
else
SOURCES_sbin += src/vdagentd/dummy-session-info.c
endif
endif

HEADERS=					\
	src/vdagentd-proto-strings.h		\
	src/vdagentd-proto.h			\
	src/udscs.h				\
	$(HEADERS_$(VARIANT))

HEADERS_bin=					\
	src/vdagent/audio.h			\
	src/vdagent/clipboard.h			\
	src/vdagent/file-xfers.h		\
	src/vdagent/x11-priv.h			\
	src/vdagent/x11.h

HEADERS_sbin=					\
	src/vdagentd/session-info.h		\
	src/vdagentd/uinput.h			\
	src/vdagentd/xorg-conf.h		\
	src/vdagentd/virtio-port.h


DEFINES=					\
	HAVE_CONFIG_H				\
	_GNU_SOURCE				\
	$(DEFINES_$(VARIANT))			\
	VERSION=\"$(PACKAGE_VERSION)\"

DEFINES_bin=					\
	UDSCS_NO_SERVER

INCLUDES=					\
	.					\
	src					\
	$(SPICE_PROTOCOL)			\
	$(INCLUDES_$(OS_NAME))

INCLUDE_macosx=

PKGCONFIGS=					\
	x11					\
	glib-2.0				\
	alsa?

CONFIG=						\
	libX11					\
	libXfixes				\
	libXinerama				\
	libXrandr				\
	getpeereid				\
	<linux/input.h>				\
	<linux/uinput.h>


ifndef SPICE_PROTOCOL
PKGCONFIGS+= spice-protocol
endif

include $(MIQ)rules.mk
$(MIQ)rules.mk:
	git clone http://github.com/c3d/make-it-quick
