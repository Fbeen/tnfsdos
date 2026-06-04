#
# TNFSDRV makefile
#

build_dir = build
src_dir   = src

cc    = wcc
asm   = wasm

cflags_tsr    = -bt=dos -ms -3 -d2 -s -zu
cflags_tools  = -bt=dos -ms -3 -d2 -s
asmflags      = -2 -ms

objs_tsr = \
	$(build_dir)/tsr.obj \
	$(build_dir)/handler.obj

all: $(build_dir)/tsr.exe $(build_dir)/showbuf.exe $(build_dir)/findtest.exe

$(build_dir):
	mkdir -p $(build_dir)

$(build_dir)/tsr.obj: $(src_dir)/tsr.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/handler.obj: $(src_dir)/handler.asm | $(build_dir)
	$(asm) $(asmflags) -fo=$@ $<

$(build_dir)/showbuf.obj: $(src_dir)/showbuf.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/tsr.exe: $(objs_tsr)
	wlink system dos \
		option map=$(build_dir)/tsr.map \
		name $@ \
		file { $(objs_tsr) }

$(build_dir)/showbuf.exe: $(build_dir)/showbuf.obj
	wlink system dos \
		option map=$(build_dir)/showbuf.map \
		name $@ \
		file { $< }

$(build_dir)/findtest.obj: $(src_dir)/findtest.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/findtest.exe: $(build_dir)/findtest.obj
	wlink system dos \
		option map=$(build_dir)/findtest.map \
		name $@ \
		file { $< }

clean:
	rm -f $(build_dir)/*.obj $(build_dir)/*.exe $(build_dir)/*.map
