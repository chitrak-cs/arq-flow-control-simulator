CC      = gcc
CFLAGS  = -Wall -Wextra -O2

COMMON  = crc.c checksum.c channel.c

all: sender receiver experiment

sender: sender.c $(COMMON)
	$(CC) $(CFLAGS) sender.c $(COMMON) -o sender

receiver: receiver.c $(COMMON)
	$(CC) $(CFLAGS) receiver.c $(COMMON) -o receiver

experiment: experiment.c $(COMMON)
	$(CC) $(CFLAGS) experiment.c $(COMMON) -o experiment

clean:
	rm -f sender receiver experiment experiment_results.csv received_output.bin
