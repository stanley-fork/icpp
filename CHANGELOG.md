## v0.1.2
Bug fixes:
 * icpp: fix optimization argument without effect error;
 * icpp: fix the standard module pcm include path error;
 * icpp: fix stub function execution error in multiple threads;
 * icpp: fix the program full path modified by llvm;

Improvements:
 * icpp: automatically generate the standard module pcm file in user local system;
 * icpp: add qemu tb buffer to 128MB per thread;
 * imod: add assets configuration to support icpp module extra payloads;
 * imod: make an os independent module if there's no extra include paths;

## v0.1.1
Bug fixes:
 * icpp: fix c source file error compiling arguments;
 * icpp: fix module main.o cache generating error;
 * icpp: fix native module handle wrong loaded order;
 * icpp: fix arm64 windows tls epochptr out of updating error;
 * icpp: fix x86_64 windows 32-bit relocation out of range overwriting error;
 * icpp: fix x86_64 relocatable jcond instruction executing error;
 * imod: fix missing calculating module data symbol hashes;
 * imod: fix dll coff file symbol parsing;

Improvements:
 * icpp: make sure argv[0] is an absolute path in icppcli driver;
 * icpp: add icpp module header directory to clang header search path;
 * icpp: add aarch64 linux ldadd4 runtime functions;
 * imod: add executable binary packing support;
 * imod: add apple framework packing support;
 * imod: add install-prefix configuration to keep packed libraries' original layout;

## v0.1.0
Initial release.
