extremely tiny 16-bit VM
not built to do anything serious, just for fun
smaller sibling to the 24c16 project

# buiding
prerequisites: 
- tup (build tool)
- any posix compliant system
- clang (gcc throws a fit)

1. run `tup` in the base directory

# use
assemble any .psm files with `./vm -a -f <input file> (-o <output file>.rom)`  
not passing in an output file places the output in a file with the same input name, but with .rom extension

run any .rom files with `./vm -v -f <input file>`  

additional options can be found in the `./vm -h` help menu
# specifications

## general CPU information
500khz clock  
custom instruction set  
16 registers  
64kb of freely addressable ram  
ability to interface with the outside universe via stdin/stdout

## memory map 
16384 bytes of rom copied into ram at 0xC000-0xFFFF
start vector is @ 0xFFFE  

## interrupts
there is one predefined interrupts, and two user-free interrupts for use with
the two free serial ports. interrupt vectors are reprogrammable as usual
[int 0 : stdin interrupt]   - vector @ 0x0000
[int 1 : user programmable] - vector @ 0x0002
[int 2 : user programmable] - vector @ 0x0004

## asm
// TODO
