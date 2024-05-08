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

## "hardware" timer interrupt
a singular interrupt may be triggered by an external, programmable clock.
the clock is set in milliseconds by writing to a specific port io


## parallel port
3 parallel ports may be used by specifying the command line argument
`-p [unix-port-loc]`. a unix port is opened at the location provided,
and issuing a connection to the port will assign the connection
to one of the three parallel ports. if all parallel ports are being used
at the time of attempting to connect, the connection will be refused
and an info log will be provided in the stdout of the VM.  

for each byte written into a parallel port from the outside, a port-related
interrupt will be called (see [hardware interrupts]). the internal state of
the parallel port will not be updated until after the interrupt is completed,
so calling `IN $index, %reg` will read the latest byte from the stream into
the required register. 

| parallel port index | io port |
|---------------------|---------|
| 0                   | 0xA0    |
| 1                   | 0xA1    |
| 2                   | 0xA2    |

## asm
// TODO
