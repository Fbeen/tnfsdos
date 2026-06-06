#
# TNFSDRV makefile
#

build_dir = build
src_dir   = src
inc_dir   = include
tcp_h_dir = ../mTCP/tcpinc
tcp_c_dir = ../mTCP/tcplib

cc    = wcc
cpp   = wpp
asm   = wasm

ifdef DEBUG
  debug_flag = -DTNFSDRV_DEBUG_RINGBUF
  all: $(build_dir)/tnfsdrv.exe $(build_dir)/dumpbuf.exe
else
  debug_flag =
  all: $(build_dir)/tnfsdrv.exe
endif

cflags_tsr   = -bt=dos -ms -3 -d2 -s -zu $(debug_flag) -I$(inc_dir)
cflags_tools = -bt=dos -ms -3 -d2 -s $(debug_flag) -I$(inc_dir)

# mTCP C++ objects and network C files: 8086, small model, no -zu
# -zp2: pack structs to 2-byte alignment (matches mTCP internals)
# -zpw: suppress packed struct warnings
cflags_mtcp = -bt=dos -ms -0 -s -zp2 -zpw $(debug_flag) \
              -DCFG_H='"tnfsdrv.cfg"' \
              -I$(tcp_h_dir) -I$(inc_dir) -I$(src_dir)

tcpobjs = \
	$(build_dir)/packet.obj \
	$(build_dir)/arp.obj \
	$(build_dir)/eth.obj \
	$(build_dir)/ip.obj \
	$(build_dir)/utils.obj \
	$(build_dir)/timer.obj \
	$(build_dir)/ipasm.obj \
	$(build_dir)/udp.obj \
	$(build_dir)/dns.obj \
	$(build_dir)/trace.obj

objs_tnfsdrv = \
	$(build_dir)/main.obj \
	$(build_dir)/config.obj \
	$(build_dir)/ringbuf.obj \
	$(build_dir)/fs_fake.obj \
	$(build_dir)/redirector.obj \
	$(build_dir)/handler.obj \
	$(build_dir)/netinit.obj \
	$(build_dir)/netw.obj \
	$(build_dir)/tnfs.obj \
	$(tcpobjs)

$(build_dir):
	mkdir -p $(build_dir)

# --- TSR core objects (with -zu) ---

$(build_dir)/main.obj: $(src_dir)/main.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/ringbuf.obj: $(src_dir)/ringbuf.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/fs_fake.obj: $(src_dir)/fs_fake.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/redirector.obj: $(src_dir)/redirector.c | $(build_dir)
	$(cc) $(cflags_tsr) -fo=$@ $<

$(build_dir)/handler.obj: $(src_dir)/handler.asm | $(build_dir)
	$(asm) -2 -ms -fo=$@ $<

# --- Non-TSR C objects (no -zu) ---

$(build_dir)/config.obj: $(src_dir)/config.c | $(build_dir)
	$(cc) $(cflags_tools) -fo=$@ $<

$(build_dir)/netinit.obj: $(src_dir)/netinit.c | $(build_dir)
	$(cc) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/tnfs.obj: $(src_dir)/tnfs.c | $(build_dir)
	$(cc) $(cflags_mtcp) -fo=$@ $<

# --- mTCP C++ object (netw.cpp) ---

$(build_dir)/netw.obj: $(src_dir)/netw.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

# --- mTCP tcplib C++ objects ---

$(build_dir)/packet.obj: $(tcp_c_dir)/packet.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/arp.obj: $(tcp_c_dir)/arp.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/eth.obj: $(tcp_c_dir)/eth.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/ip.obj: $(tcp_c_dir)/ip.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/utils.obj: $(tcp_c_dir)/utils.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/timer.obj: $(tcp_c_dir)/timer.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/ipasm.obj: $(tcp_c_dir)/ipasm.asm | $(build_dir)
	$(asm) -0 -ms -fo=$@ $<

$(build_dir)/udp.obj: $(tcp_c_dir)/udp.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/dns.obj: $(tcp_c_dir)/dns.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

$(build_dir)/trace.obj: $(tcp_c_dir)/trace.cpp | $(build_dir)
	$(cpp) $(cflags_mtcp) -fo=$@ $<

# --- Tools ---

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

clean:
	rm -f $(build_dir)/*.obj $(build_dir)/*.exe $(build_dir)/*.map
