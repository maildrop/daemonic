
if DEBUG
CFLAGS += 
else
CFLAGS += -DNDEBUG
endif

bin_PROGRAMS = daemonic
noinst_PROGRAMS = sampledaemon execpath
daemonic_SOURCES = daemonic.c verify.h
sampledaemon_SOURCES = sampledaemon.c
execpath_SOURCES = execpath.c verify.h

.PHONY: emacsclean
clean: emacsclean clean-am
emacsclean:
	find . -name "*~" -exec rm {} \;
