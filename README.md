# PlcFsmVis
Convert's a TC (german company of PLC's) .TcPOU files to pure ascii.
It can then look for things that look like a FSM, and visulize them using 
graphviz.

# build instructions
mkdir build; cd build; cmake ..; make

# run instructions
$ ./tcpou2st test.TcPOU  test.st
$ ./stvis test.st  >graph.dot
$ dot -Tpng graph.dot >graph.png

