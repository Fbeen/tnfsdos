#
# TNFSDRV makefile — packet-driver backend
#
# Build targets:
#   make DEBUG=1   — packet-driver, debug ring-buffer  [primary dev build]
#   make           — packet-driver, release
#

build_dir = build
src_dir   = src
inc_dir   = include

cc  = wcc
asm = wasm

ifdef DEBUG
  debug_flag = -DTNFSDRV_DEBUG_RINGBUF
  all: $(build_dir)/tnfsdrv.exe $(build_dir)/dumpbuf.exe $(build_dir)/pdrel.exe
else
  debug_flag =
  all: $(build_dir)/tnfsdrv.exe
endif

FS_OBJ       = $(build_dir)/fs_tnfsmin.obj
network_objs = \
	$(build_dir)/netinit.obj \
	$(build_dir)/tnfs.obj \
	$(build_dir)/netw_pd.obj \
	$(build_dir)/netw_pd_rcv.obj

cflags_tsr   = -bt=dos -ms -3 -d2 -s -zu $(debug_flag) -I$(inc_dir)
cflags_tools = -bt=dos -ms -3 -d2 -s    $(debug_flag) -I$(inc_dir)

objs_tnfsdrv = \
	$(build_dir)/main.obj \
	$(build_dir)/config.obj \
	$(build_dir)/ringbuf.obj \
	$(FS_OBJ) \
	$(build_dir)/redirector.obj \
	$(build_dir)/handler.obj \
	$(network_objs)

$(build_dir):
	mkdir -p $(build_dir)

# --- TSR core objects (compiled with -zu: DS != SS in handler context) ---

$(build_dir)/main.obj: $(src_dir)/main.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/ringbuf.obj: $(src_dir)/ringbuf.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/fs_tnfsmin.obj: $(src_dir)/fs_tnfsmin.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/redirector.obj: $(src_dir)/redirector.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/handler.obj: $(src_dir)/handler.asm | $(build_dir)
	$(asm) -2 -ms -fo=$@ $<

$(build_dir)/tnfs.obj: $(src_dir)/tnfs.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/netw_pd.obj: $(src_dir)/netw_pd.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/netw_pd_rcv.obj: $(src_dir)/netw_pd_rcv.asm | $(build_dir)
	$(asm) -2 -ms -fo=$@ $<

# --- Non-TSR objects (no -zu) ---

$(build_dir)/config.obj: $(src_dir)/config.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/netinit.obj: $(src_dir)/netinit.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/dumpbuf.obj: $(src_dir)/dumpbuf.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

# --- Link ---

$(build_dir)/tnfsdrv.exe: $(objs_tnfsdrv)
	wlink system dos \
		option stack=4096 \
		option map=$(build_dir)/tnfsdrv.map \
		name $@ \
		file { $(objs_tnfsdrv) }

$(build_dir)/dumpbuf.exe: $(build_dir)/dumpbuf.obj
	wlink system dos \
		option map=$(build_dir)/dumpbuf.map \
		name $@ \
		file { $< }

$(build_dir)/pdrel.obj: $(src_dir)/pdrel.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/pdrel.exe: $(build_dir)/pdrel.obj
	wlink system dos \
		option map=$(build_dir)/pdrel.map \
		name $@ \
		file { $< }

clean:
	rm -f $(build_dir)/*.obj $(build_dir)/*.exe $(build_dir)/*.map $(build_dir)/*.err
