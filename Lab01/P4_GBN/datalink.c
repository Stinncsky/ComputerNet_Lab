#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 265 * NR_BUFS + 540  //累计ACK超时时间
#define DEBUG

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



struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};

static unsigned char frame_nr = 0, send_buffer[NR_BUFS + 1][PKT_LEN], nbuffered = 0, recv_buffer[PKT_LEN];
static unsigned char frame_expected = 0,ack_expected = 0, next_frame_to_send = 0;
static int phl_ready = 0;
//序号增加
static void inc(unsigned char *p)
{
	*p = (*p + 1) % (NR_BUFS + 1);
}

//判断是否在窗口内
static unsigned char between(unsigned int a, unsigned int b, unsigned int c)
{
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((c < a) && (b < c));
}

//成帧
static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);//CRC校验
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_data_frame(unsigned char fk, unsigned char frame_nr, unsigned char frame_expected, unsigned char buffer[NR_BUFS][PKT_LEN])
{
	struct FRAME s;

	s.kind = fk;
	switch(fk) {
		case FRAME_DATA:
			s.seq = frame_nr;
            s.ack = -1;
			memcpy(s.data, buffer[frame_nr % (NR_BUFS + 1)], PKT_LEN);
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_frame("Send DATA %d ID %d\n", s.seq, *(short *)s.data);

			put_frame((unsigned char *)&s, 3 + PKT_LEN);
			start_timer(frame_nr, DATA_TIMER);
			break;
		case FRAME_ACK://无捎带确认
			s.ack = (frame_expected + NR_BUFS) % (NR_BUFS + 1);//接收方期望的帧号的前一个帧号已经接收，可以确认
			//Debug输出
			set_text_color(FOREGROUND_RED);
			dbg_frame("Send ACK  %d\n", s.ack);
			
			put_frame((unsigned char *)&s, 2);
			break;
	}
}



int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("build: " __DATE__ "  "__TIME__"\n");

	disable_network_layer();

	while(1) {
		event = wait_for_event(&arg);

		switch (event) {
		case NETWORK_LAYER_READY:
			get_packet(send_buffer[next_frame_to_send]);//从网络层获取数据
			nbuffered++;//窗口右移
			send_data_frame(FRAME_DATA, next_frame_to_send, frame_expected, send_buffer);
			inc(&next_frame_to_send);
            //dbg_event("---- DATA %d  %d  %d sent\n", next_frame_to_send, ack_expected, frame_expected);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char *)&f, sizeof f);
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                set_text_color(FOREGROUND_GREEN);
				dbg_event("****Error, Bad CRC Checksum\n");
				break;
			}
			if (f.kind == FRAME_ACK){

                while (between(ack_expected, f.ack, next_frame_to_send)){
                    nbuffered--;//接收到ACK，窗口右移
                    //Debug输出
                    set_text_color(FOREGROUND_GREEN);
                    dbg_frame("Recv ACK  %d\n", f.ack);
                    stop_timer(ack_expected);
                    inc(&ack_expected);
                }
            }
                
			
            if (f.kind == FRAME_DATA) {
                set_text_color(FOREGROUND_GREEN);
				dbg_frame("Recv DATA %d frame_exp %d\n", f.seq, frame_expected);
				
                if (f.seq == frame_expected) {
                    memcpy(recv_buffer, f.data, PKT_LEN);
                    put_packet(recv_buffer, len - 7);
                    inc(&frame_expected);
                    //dbg_event("---- DATA %d %d accepted\n", f.seq, frame_expected);
                    send_data_frame(FRAME_ACK, 0, frame_expected, send_buffer);
                }
			}
			break;

		case DATA_TIMEOUT:
            set_text_color(FOREGROUND_RED);
			dbg_event("---- DATA %d timeout\n", arg);
            next_frame_to_send = ack_expected;
			for(int i = 1; i <= nbuffered; i++){
				send_data_frame(FRAME_DATA, next_frame_to_send, frame_expected, send_buffer);
                inc(&next_frame_to_send);             
            }

			break;
		}

		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}
}