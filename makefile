# make rule primaria con dummy target ‘all’--> non crea alcun file all ma fa un complete build
# 											   che dipende dai target client e server scritti sotto
all: 		dev serv

# make rule per il device
dev:		device.o
			gcc device.o -o dev

device.o: 	device.c
			gcc -Wall -c device.c

# make rule per il server
serv:		server.o
			gcc server.o -o serv
server.o:	server.c
			gcc -Wall -c server.c

# pulizia
clean:
			rm *.o dev serv