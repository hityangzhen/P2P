# Define C++ compiler to be used
CXX=g++

# Define flags to be passed to the C compiler
CXXFLAGS=-Wall -pedantic -std=gnu99 -g

# Define flags for libraries to be linked when compiling the application
LDFLAGS=-lpthread -lsqlite3

# Define the name of the output program
PROG=napd

# Define the name of the main configuration header
CFG=config.h

# Define the main of the main module
MAIN=main

# Define the name of the threadpool module
TP=thpool
#---------- MAKEFILE -------------------

${PROG}:	${MAIN}.o ${TP}.o
		${CXX} ${MAIN}.o ${TP}.o -o ${PROG} ${LDFLAGS}
		rm *.o

${MAIN}.o:	${MAIN}.c ${CFG}
		${CXX} ${CFLAGS} -c ${MAIN}.c -o ${MAIN}.o

${TP}.o:	${TP}.cc ${TP}.h
		${CXX} ${CFLAGS} -c ${TP}.cc -o ${TP}.o

clean:
		rm ${PROG} *.o
