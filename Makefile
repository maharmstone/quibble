all: quibblex86 quibbleamd64

quibblex86:
	$(MAKE) -C x86

quibbleamd64:
	$(MAKE) -C amd64

clean:
	$(MAKE) -C x86 clean
	$(MAKE) -C amd64 clean

