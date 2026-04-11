CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS_THREAD = -lpthread -lm

.PHONY: all ping iperf traceroute clean

all: ping iperf traceroute

ping: ping/server ping/client

ping/server: ping/server.c
	$(CC) $(CFLAGS) -o $@ $<

ping/client: ping/client.c
	$(CC) $(CFLAGS) -o $@ $<

iperf: iperf/iperf_client

iperf/iperf_client: iperf/iperf_client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_THREAD)

traceroute: traceroute/traceroute

traceroute/traceroute: traceroute/traceroute.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ping/server ping/client iperf/iperf_client traceroute/traceroute \
	      iperf/throughput_stats.csv iperf/throughput_plot.png