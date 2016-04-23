
bin_PROGRAMS = daemonic
noinst_PROGRAMS = sampledaemon
daemonic_SOURCES = daemonic.c
daemonic_CFLAGS = -D_XOPEN_SOURCE=700 
sampledaemon_SOURCES = sampledaemon.c
sampledaemon_CFLAGS = -D_XOPEN_SOURCE=700 

.PHONY: emacsclean
clean: emacsclean clean-am
emacsclean:
	find . -name "*~" -exec rm {} \;
