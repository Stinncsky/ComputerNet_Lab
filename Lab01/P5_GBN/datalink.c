#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"
#define ACK_TIMER 276
#define DATA_TIMER 263 * SEND_BUFS + ACK_TIMER + SEND_BUFS - 1

#ifdef _WIN32
	#include <conio.h>
	#include <windows.h>
#else
	#define FOREGROUND_GREEN 0x2
	#define FOREGROUND_RED 0x4
#endif

#if defined(DEBUG) && defined(_WIN32)
void set_text_color(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}
#else
void set_text_color(int color) {}
#endif

#define DEBUG_PRINT 1

struct FRAME {
	unsigned char kind; /* FRAME_DATA TYPE */
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};

static unsigned char nbuffered = 0, send_buffer[SEND_BUFS + 1][PKT_LEN], recv_buffer[PKT_LEN];
static unsigned char no_nak = 1;
static unsigned char frame_expected = 0, ack_expected = 0, next_frame_to_send = 0;
static int phl_ready = 0;

static unsigned char between(unsigned int a, unsigned int b, unsigned int c){
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((c < a) && (b < c));
}

static void inc(unsigned char *a){
	*a = ((*a) + 1) % (MAX_SEQ + 1);
}

static void put_frame(unsigned char *frame, int len){
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_frame_to_p(unsigned char frame_kind, unsigned char frame_seq, unsigned char frame_expected, unsigned char buffer[SEND_BUFS][PKT_LEN]){
	struct FRAME s;
	s.kind = frame_kind;
	
	switch (frame_kind) {
		case FRAME_DATA:
			s.seq = frame_seq;
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
			memcpy(s.data, buffer[frame_seq], PKT_LEN);
			#if DEBUG_PRINT
				set_text_color(FOREGROUND_RED);
				dbg_frame("Send DATA %d ACK %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
			#endif
			put_frame((unsigned char *)&s, 3 + PKT_LEN);
			start_timer(frame_seq, DATA_TIMER);
			break;
		case FRAME_ACK:
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
			#if DEBUG_PRINT
				set_text_color(FOREGROUND_RED);
				dbg_frame("Send ACK  %d\n", s.ack);
			#endif
			put_frame((unsigned char *)&s, 2);
			break;
		case FRAME_NAK:
			no_nak = 0;
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
			#if DEBUG_PRINT
				set_text_color(FOREGROUND_RED);
				dbg_frame("Send NAK  %d\n", frame_expected);
			#endif
			put_frame((unsigned char *)&s, 2);
			break;
	}
	stop_ack_timer();
}

int main(int argc, char **argv){
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("build: " __DATE__ "  "__TIME__"\n");

	disable_network_layer();

	while (1) {
		event = wait_for_event(&arg);

		switch (event) {
		case NETWORK_LAYER_READY:
			get_packet(send_buffer[next_frame_to_send]);
			nbuffered++;
			send_frame_to_p(FRAME_DATA, next_frame_to_send, frame_expected, send_buffer);
			inc(&next_frame_to_send);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char *)&f, sizeof f);
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
				#if DEBUG_PRINT
					set_text_color(FOREGROUND_GREEN);
					dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				#endif
				if (no_nak){
					send_frame_to_p(FRAME_NAK, 0, frame_expected, send_buffer);
				}
				break;
			}

			if (f.kind == FRAME_ACK) {
				#if DEBUG_PRINT
					set_text_color(FOREGROUND_GREEN),dbg_frame("Recv ACK  %d\n", f.ack);
				#endif
			}

			if (f.kind == FRAME_DATA) {
				#if DEBUG_PRINT
					set_text_color(FOREGROUND_GREEN);
					dbg_frame("Recv DATA %d frame_exp %d\n", f.seq, frame_expected);
				#endif
				if (f.seq != frame_expected && no_nak){
					send_frame_to_p(FRAME_NAK, 0, frame_expected, send_buffer);
				}
				else
					start_ack_timer(ACK_TIMER);
				if (frame_expected == f.seq) {
					memcpy(recv_buffer, f.data, PKT_LEN);
					#if DEBUG_PRINT
						set_text_color(FOREGROUND_GREEN);
						dbg_frame("---- Recv DATA %d\n", f.seq);
						dbg_event("---- Upto--NETWORK %d\n", frame_expected);
					#endif
					put_packet(recv_buffer, len - 7);
					no_nak = 1;
					inc(&frame_expected);
					start_ack_timer(ACK_TIMER);
				}
			}

			if (f.kind == FRAME_NAK && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
				#if DEBUG_PRINT
					set_text_color(FOREGROUND_GREEN);
					dbg_frame("Recv NAK  %d\n", (f.ack + 1) % (MAX_SEQ + 1));
				#endif
				for(int i = (f.ack + 1) % (MAX_SEQ + 1); i != next_frame_to_send; i = (i + 1) % (MAX_SEQ + 1))
					send_frame_to_p(FRAME_DATA, i, frame_expected, send_buffer);
			}

			while (between(ack_expected, f.ack, next_frame_to_send)) {
				nbuffered--;
				#if DEBUG_PRINT
					set_text_color(FOREGROUND_RED);
					dbg_frame("Recv toN--ACK  %d\n", ack_expected);
				#endif
				stop_timer(ack_expected);
				inc(&ack_expected);
			}
			break;

		case DATA_TIMEOUT:
			#if DEBUG_PRINT
				set_text_color(FOREGROUND_RED);
				dbg_event("----DATA %d timeout\n", arg);
			#endif
			for(int i = arg; i != next_frame_to_send; i = (i + 1) % (MAX_SEQ + 1))
				send_frame_to_p(FRAME_DATA, i, frame_expected, send_buffer);
			break;

		case ACK_TIMEOUT:
			#if DEBUG_PRINT
				set_text_color(FOREGROUND_GREEN);
				dbg_event("----ACK timeout\n");
			#endif
			send_frame_to_p(FRAME_ACK, 0, frame_expected, send_buffer);
			break;
		}

		if (nbuffered < SEND_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}
}