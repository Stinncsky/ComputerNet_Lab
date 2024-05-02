#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"
#define ACK_TIMER 276
#define DATA_TIMER 263 * NR_BUFS + ACK_TIMER + NR_BUFS - 1

#if defined(DEBUG) && defined(_WIN32)
void set_text_color(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}
#else
void set_text_color(int color) {}
#endif

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#define FOREGROUND_GREEN 0x2
#define FOREGROUND_RED 0x4
#endif

struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};

static unsigned char nbuffered = 0, send_buffer[NR_BUFS][PKT_LEN], recv_buffer[NR_BUFS][PKT_LEN];
static unsigned char no_nak = 1;
static unsigned char frame_expected = 0, recv_frame_UB = NR_BUFS, ack_expected = 0, arrived[NR_BUFS], next_frame_to_send = 0;
static int phl_ready = 0;

//判断是否在窗口内
static unsigned char between(unsigned int a, unsigned int b, unsigned int c)
{
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((c < a) && (b < c));
}

//序号增加
static void inc(unsigned char *a)
{
	*a = ((*a) + 1) % (MAX_SEQ + 1);
}

//成帧并发送
static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_frame_to_p(unsigned char fk, unsigned char frame_nr, unsigned char frame_expected, unsigned char buffer[NR_BUFS][PKT_LEN])
{
	struct FRAME s;

	s.kind = fk;
	switch (fk) {
		case FRAME_DATA:
			s.seq = frame_nr;
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);//接收方期望的帧号的前一个帧号已经接收，可以确认
			memcpy(s.data, buffer[frame_nr % NR_BUFS], PKT_LEN);
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_frame("SENDER Send DATA %d ACK %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
			//
			put_frame((unsigned char *)&s, 3 + PKT_LEN);
			start_timer(frame_nr, DATA_TIMER);
			break;
		case FRAME_ACK:
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);//接收方期望的帧号的前一个帧号已经接收，可以确认
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_frame("SENDER Send ACK  %d\n", s.ack);
			//
			put_frame((unsigned char *)&s, 2);
			break;
		case FRAME_NAK:
			no_nak = 0;
			s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);//接收方期望的帧号的前一个帧号已经接收，可以确认
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_frame("SENDER Send NAK  %d\n", frame_expected);
			//
			put_frame((unsigned char *)&s, 2);
			break;
	}
	//发送帧后，就会捎带ACK，停止ACK计时器
	stop_ack_timer();
}	
int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("build: " __DATE__ "  "__TIME__"\n");

	disable_network_layer();

	for (int i = 0; i < NR_BUFS; i++)
		arrived[i] = 0;

	while (1) {
		event = wait_for_event(&arg);

		switch (event) {
		case NETWORK_LAYER_READY:
			get_packet(send_buffer[next_frame_to_send % NR_BUFS]);
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
				//Debug输出
				set_text_color(FOREGROUND_GREEN);
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				//
				if (no_nak)
					send_frame_to_p(FRAME_NAK, 0, frame_expected, send_buffer);//帧错误，发送NAK，要求重发预期帧
				break;
			}

			if (f.kind == FRAME_ACK)
				//Debug输出
				set_text_color(FOREGROUND_GREEN),dbg_frame("RECEIVER Recv ACK  %d\n", f.ack);
			
			if (f.kind == FRAME_DATA) {
				//Debug输出
				set_text_color(FOREGROUND_GREEN);
				dbg_frame("RECEIVER  Recv DATA %d frame_exp %d\n", f.seq, frame_expected);
				//
				if (f.seq != frame_expected && no_nak)
					send_frame_to_p(FRAME_NAK, 0, frame_expected, send_buffer);//接收到的帧号不是期望的帧号，发送NAK
				else
					start_ack_timer(ACK_TIMER);
				if (between(frame_expected, f.seq, recv_frame_UB) && arrived[f.seq % NR_BUFS] == 0) {
					arrived[f.seq % NR_BUFS] = 1;
					memcpy(recv_buffer[f.seq % NR_BUFS], f.data, PKT_LEN);
					while (arrived[frame_expected % NR_BUFS]) {//RECEIVER 滑动窗口
						//Debug输出
						set_text_color(FOREGROUND_GREEN);
						dbg_frame("---- Recv DATA %d\n", f.seq);
						dbg_event("---- RECEIVER Upto--NETWORK %d\n", frame_expected);
						//
						put_packet(recv_buffer[frame_expected % NR_BUFS], len - 7);
						arrived[frame_expected % NR_BUFS] = 0;
						no_nak = 1;
						inc(&frame_expected);
						inc(&recv_frame_UB);
						start_ack_timer(ACK_TIMER);
					}
				}
			}

			if (f.kind == FRAME_NAK && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
				//Debug输出
				set_text_color(FOREGROUND_GREEN);
				dbg_frame("SENDER Recv NAK  %d\n", (f.ack + 1) % (MAX_SEQ + 1));
				//
				send_frame_to_p(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected, send_buffer);
			}

			while (between(ack_expected, f.ack, next_frame_to_send)) {//SENDER滑动窗口
				nbuffered--;
				//Debug输出
				set_text_color(FOREGROUND_RED);
				dbg_frame("SENDER Recv toN--ACK  %d\n", ack_expected);
				//
				stop_timer(ack_expected);
				inc(&ack_expected);
			}
			break;

		case DATA_TIMEOUT:
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_event("----SENDER DATA %d timeout\n", arg);
			//
			send_frame_to_p(FRAME_DATA, arg, frame_expected, send_buffer);
			break;
		case ACK_TIMEOUT:
			//Debug输出
			set_text_color(FOREGROUND_GREEN);
			dbg_event("----RECEIVER ACK timeout\n");
			//
			send_frame_to_p(FRAME_ACK, 0, frame_expected, send_buffer);
			break;
		}

		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}
}