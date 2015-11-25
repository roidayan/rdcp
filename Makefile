CC = gcc
CFLAGS = -ggdb
# -Wall
LDFLAGS = -libverbs -lrdmacm -lpthread
XSLTPROC = /usr/bin/xsltproc

all: rdcp rdcp.8
default: all

.PHONY: rdcp.8
rdcp.8: rdcp.8.xml
	-test -z "$(XSLTPROC)" || $(XSLTPROC) -o $@ http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

.PHONY: clean
clean:
	rm -f rdcp
	rm -f rdcp.8

rdcp:
