extremely tiny 16-bit VM
not built to do anything serious, just for fun
smaller sibling to the 24c16 project


- vm specifications
16384 bytes of rom copied into ram at 0xC000-0xFFFF
start vector is @ 0xFFFE
500khz clock

there are two predefined interrupts, and one user-free interrupt
interrupt vectors are programmable
[int 0 : stdin interrupt]   - vector @ 0x0000
[int 1 : user programmable] - vector @ 0x0002
[int 2 : user programmable] - lookup @ 0x0004

- asm
primitives: 
	integer literals/addresses
	dereferenced addresses
	labels (a named literal/address)
