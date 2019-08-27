/*
	grbl_cnc.c

	Copyright 2019 Vladimir Alatartsev <svhelper@mail.ru>
	Copyright 2008-2011 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __MINGW32__
# error This is Windows only code
#endif /*__MINGW32__*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <pthread.h>

#include <windows.h>
#include "fifo_declare.h"
#include "avr_uart.h"
#include "avr_timer.h"
#include "avr_ioport.h"

#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_gdb.h"
#include "sim_vcd_file.h"


avr_t * avr = NULL;
avr_vcd_t vcd_file;

enum {
	IRQ_UART_BRIDGE_BYTE_IN = 0,
	IRQ_UART_BRIDGE_BYTE_OUT,
	IRQ_UART_BRIDGE_COUNT
};

static const char * irq_names[IRQ_UART_BRIDGE_COUNT] = {
	[IRQ_UART_BRIDGE_BYTE_IN] = "8<uart_bridge.in",
	[IRQ_UART_BRIDGE_BYTE_OUT] = "8>uart_bridge.out",
};

DECLARE_FIFO(uint8_t, uart_bridge_fifo, 512);

typedef struct uart_bridge_t {
	avr_t     * avr;		// keep it around so we can pause it
	avr_irq_t *	irq;		// irq list

	pthread_t	thread;
	HANDLE		s;
	uint8_t		xon;

	uart_bridge_fifo_t in;
	uart_bridge_fifo_t out;
} uart_bridge_t;

uart_bridge_t ub;

DEFINE_FIFO(uint8_t, uart_bridge_fifo);


HANDLE fifo_write_event;

/*
 * called when a byte is send via the uart on the AVR
 */
static void uart_bridge_in_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	uart_bridge_t * p = (uart_bridge_t*)param;
	//printf("%s %02x\n", __FUNCTION__, value);
	uart_bridge_fifo_write(&p->in, value);
	SetEvent(fifo_write_event);
}


/*
 * Called when the uart has room in it's input buffer. This is called repeateadly
 * if necessary, while the xoff is called only when the uart fifo is FULL
 */
static void
uart_bridge_xon_hook(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	uart_bridge_t * p = (uart_bridge_t *)param;
	p->xon = value;
}

/*
 * Called when the uart ran out of room in it's input buffer
 */
static void
uart_bridge_xoff_hook(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	uart_bridge_t * p = (uart_bridge_t *)param;
	p->xon = !value;
}

/*
 * Serial port routine
 */
static void * uart_bridge_thread(void * param)
{
	uart_bridge_t * p = (uart_bridge_t*)param;
	DWORD dw;
	unsigned len = 0;
	uint8_t buffer_rx[512 - 1]; OVERLAPPED ovl_rx = {0};
	uint8_t buffer_tx[512 - 1]; OVERLAPPED ovl_tx = {0};
	uint8_t *src;

	enum {
		EVT_RX = 0,
		EVT_TX,
		EVT_IN,
		EVT_COUNT
	};
	HANDLE evts[EVT_COUNT] = {
		[EVT_RX] = CreateEventA(NULL, FALSE, FALSE, NULL),
		[EVT_TX] = CreateEventA(NULL, FALSE, FALSE, NULL),
		[EVT_IN] = fifo_write_event,
	};
	unsigned wait_evts_num;

	ovl_rx.hEvent = evts[EVT_RX];
	ovl_tx.hEvent = evts[EVT_TX];

	dw = 0;
	if (!ReadFile(p->s, buffer_rx, sizeof(buffer_rx), &dw, &ovl_rx) && GetLastError() != ERROR_IO_PENDING) {
		fprintf(stderr, "UART BRIDGE read failed\n");
		goto failure_;
	}
	wait_evts_num = EVT_COUNT;

	while (1) {
		switch(WaitForMultipleObjects(wait_evts_num, evts, FALSE, INFINITE))
		{
			case WAIT_OBJECT_0 + EVT_RX:
				GetOverlappedResult(p->s, &ovl_rx, &dw, TRUE);
				// hdump("uart bridge recv", buffer, r);

				// write them in fifo
				src = buffer_rx;
				while (dw && !uart_bridge_fifo_isfull(&p->out)) {
					dw --; uart_bridge_fifo_write(&p->out, *src++);
				}
				if (dw)
					fprintf(stderr, "UART BRIDGE dropped %u bytes\n", (unsigned int)dw);

				dw = 0;
				if (!ReadFile(p->s, buffer_rx, sizeof(buffer_rx), &dw, &ovl_rx) && GetLastError() != ERROR_IO_PENDING) {
					fprintf(stderr, "UART BRIDGE read failed\n");
					goto failure_;
				}
				break;

			case WAIT_OBJECT_0 + EVT_TX:
				GetOverlappedResult(p->s, &ovl_tx, &dw, TRUE);
				wait_evts_num = EVT_COUNT;
			case WAIT_OBJECT_0 + EVT_IN:
				ResetEvent(fifo_write_event);

				if (wait_evts_num != EVT_COUNT) {
					fprintf(stderr, "UART BRIDGE wrong state\n");
					goto failure_;
				}

				if (!uart_bridge_fifo_isempty(&p->in))
				{
					len = 0;
					src = buffer_tx;
					while(len<512 && !uart_bridge_fifo_isempty(&p->in)) {
						len++; *src++ = uart_bridge_fifo_read(&p->in);
					}

					src = buffer_tx;
					dw = 0;
					if (!WriteFile(p->s, buffer_tx, len, &dw, &ovl_tx) && GetLastError() != ERROR_IO_PENDING) {
						fprintf(stderr, "UART BRIDGE write failed\n");
						goto failure_;
					}
					wait_evts_num = EVT_COUNT - 1;
				}
				break;

			default:
				fprintf(stderr, "UART BRIDGE wait failed\n");
				goto failure_;
		}
	}

failure_:
	CloseHandle(evts[EVT_RX]);
	CloseHandle(evts[EVT_TX]);
	printf("UART BRIDGE exit working thread\n");
	return NULL;
}

static void uart_bridge_init(struct avr_t * avr, uart_bridge_t * p, const char *serial_port)
{
	p->avr = avr;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_BRIDGE_COUNT, irq_names);
	avr_irq_register_notify(p->irq + IRQ_UART_BRIDGE_BYTE_IN, uart_bridge_in_hook, p);

	if ((p->s = CreateFileA(serial_port, GENERIC_READ | GENERIC_WRITE, 0, NULL, 
							OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
							NULL)) == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "%s: Can't open serial port: %s", __FUNCTION__, serial_port);
		return;
	}

	DCB dcb = { sizeof(dcb), };
	if (!GetCommState(p->s, &dcb)) {
		fprintf(stderr, "%s: Can't get serial port config: %s", __FUNCTION__, serial_port);
		return;
	}
	dcb.fBinary = 1;
	dcb.BaudRate = 115200;
	dcb.ByteSize = 8;
	dcb.StopBits = 1;
	dcb.Parity = 0;
	if (!SetCommState(p->s, &dcb)) {
		fprintf(stderr, "%s: Can't update serial port config: %s", __FUNCTION__, serial_port);
		return;
	}
	COMMTIMEOUTS to = {0};
	to.ReadIntervalTimeout = 1;
	to.ReadTotalTimeoutConstant = INFINITE;
	to.ReadTotalTimeoutMultiplier = INFINITE;
	to.WriteTotalTimeoutConstant = 0;
	to.WriteTotalTimeoutMultiplier = INFINITE;
	if (!SetCommTimeouts(p->s, &to)) {
		fprintf(stderr, "%s: Can't update serial port timeouts: %s", __FUNCTION__, serial_port);
		return;
	}

	fifo_write_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	pthread_create(&p->thread, NULL, uart_bridge_thread, p);

}

static void uart_bridge_connect(uart_bridge_t * p, char uart)
{
	// disable the stdio dump, as we are sending binary there
	uint32_t f = 0;
	avr_ioctl(p->avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
	f &= ~AVR_UART_FLAG_STDIO;
	avr_ioctl(p->avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

	avr_irq_t * src = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
	avr_irq_t * dst = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
	avr_irq_t * xon = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
	avr_irq_t * xoff = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);
	if (src && dst) {
		avr_connect_irq(src, p->irq + IRQ_UART_BRIDGE_BYTE_IN);
		avr_connect_irq(p->irq + IRQ_UART_BRIDGE_BYTE_OUT, dst);
	}
	if (xon)
		avr_irq_register_notify(xon, uart_bridge_xon_hook, p);
	if (xoff)
		avr_irq_register_notify(xoff, uart_bridge_xoff_hook, p);
}




static int firmware_var_varptr(elf_firmware_t *f, const char* symname)
{
	avr_symbol_t **s = f->symbol;
	for(uint32_t i=f->symbolcount; i; i--, s++)
	{
		if (strcmp(symname, (*s)->symbol))
			continue;
		return (*s)->addr;
	}
	return -1;
}

static void* firmware_get_varptr(elf_firmware_t *f, const char* symname)
{
	int addr = firmware_var_varptr(f, symname);
	if (addr == -1)
		return NULL;
	
	addr &= 0xffffff;
	if (addr < avr->flashend) {
		return avr->flash + addr;
	}
	if (addr >= 0x800000 && (addr - 0x800000) <= avr->ramend) {
		return avr->data + addr - 0x800000;
	}
//	if (addr >= 0x810000 && (addr - 0x810000) <= avr->e2end) {
//		avr_eeprom_desc_t ee = {.offset = (addr - 0x810000)};
//		avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &ee);
//		if (ee.ee)
//			src = ee.ee;
//	}

	return NULL;
}


#define N_AXIS 3 // Number of axes
#define X_AXIS_IND      0
#define Y_AXIS_IND      1
#define Z_AXIS_IND      2

#define STEP_PORT       'D'
#define X_STEP_BIT      2  // Uno Digital Pin 2
#define Y_STEP_BIT      3  // Uno Digital Pin 3
#define Z_STEP_BIT      4  // Uno Digital Pin 4


#define _SFR_IO8(v) ((v)+32)		//#include "../simavr/cores/sim_core_declare.h"
#define PORTD _SFR_IO8(0x0B)
#define DIRECTION_PORT    PORTD
#define X_DIRECTION_BIT   5  // Uno Digital Pin 5
#define Y_DIRECTION_BIT   6  // Uno Digital Pin 6
#define Z_DIRECTION_BIT   7  // Uno Digital Pin 7

avr_regbit_t axis_reg[N_AXIS] = {
	{ .reg = DIRECTION_PORT, .bit = X_DIRECTION_BIT, .mask = 1 },
	{ .reg = DIRECTION_PORT, .bit = Y_DIRECTION_BIT, .mask = 1 },
	{ .reg = DIRECTION_PORT, .bit = Z_DIRECTION_BIT, .mask = 1 },
};


static void
axis_irq_handler(
		struct avr_irq_t * irq,
		uint32_t value,
        void *param)
{
	if (value)
		return;
	uint8_t dir = avr_regbit_get(avr, axis_reg[irq->irq]);
	uint32_t *b = (uint32_t *) param;
	if (dir) (*b)--;
	else (*b)++;
}



int main(int argc, char *argv[])
{
	const char *app_path = argv[0];
	elf_firmware_t f;
	const char * fname =  "grbl.axf";

	{
		typedef MMRESULT (WINAPI *timeBeginPeriod_t)(UINT);
		timeBeginPeriod_t _timeBeginPeriod;
		_timeBeginPeriod = (timeBeginPeriod_t)GetProcAddress(LoadLibraryA("Winmm.dll"), "timeBeginPeriod");
		// Ask for possibility to use Sleep on 1ms delay (or at least delay less than 16ms)
		_timeBeginPeriod(1);
	}

	printf("Firmware pathname is %s\n", fname);
	elf_read_firmware(fname, &f);

	// GRBL configuration
	f.frequency = 16000000UL;
	strcpy(f.mmcu, "atmega328p");
	f.vcc = f.avcc = f.aref = 5000;

	printf("firmware %s f=%d mmcu=%s\n", fname, (int)f.frequency, f.mmcu);

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", app_path, f.mmcu);
		exit(1);
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);
	//avr->log = LOG_DEBUG;
	//avr->trace = 1;
	//int trace = avr_timer_trace_ocr | avr_timer_trace_tcnt | avr_timer_trace_compa | avr_timer_trace_compb | avr_timer_trace_compc;
	//avr_ioctl(avr, AVR_IOCTL_TIMER_SET_TRACE('0'), &trace);
	//avr_ioctl(avr, AVR_IOCTL_TIMER_SET_TRACE('1'), &trace);

	// even if not setup at startup, activate gdb if crashing
	avr->gdb_port = 1234;
	if (argc >= 2 && !strcmp(argv[1], "-g")) {
		printf("Starting GDB server...\n");
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
		argc --; argv ++;
	}

	if (argc >= 2) {
		printf("Attaching UART%c to %s ...\n", '0', argv[1]);
		uart_bridge_init(avr, &ub, argv[1]);
		uart_bridge_connect(&ub, '0');
		argc --; argv ++;
	}

	int32_t irq_position[N_AXIS] = {-1,-1,-1};
	int32_t sys_position_last[N_AXIS];      // Real-time machine (aka home) position vector in steps.
	int32_t sys_position_m[N_AXIS];      // Real-time machine (aka home) position vector in steps.
	int32_t sys_position_c;

	int32_t *sys_position = firmware_get_varptr(&f, "sys_position");				// int32_t sys_position[N_AXIS];      // Real-time machine (aka home) position vector in steps.
	printf("%s mapped @ 0x%p\n", "sys_position", sys_position);

	static const char * irq_names[N_AXIS] = {
		[X_AXIS_IND] = "=X",
		[Y_AXIS_IND] = "=Y",
		[Z_AXIS_IND] = "=Z",
	};
	avr_irq_t *axis_irq = avr_alloc_irq(&avr->irq_pool, 0, N_AXIS, irq_names);
	avr_irq_register_notify(axis_irq + X_AXIS_IND, axis_irq_handler, &irq_position[X_AXIS_IND]);
	avr_irq_register_notify(axis_irq + Y_AXIS_IND, axis_irq_handler, &irq_position[Y_AXIS_IND]);
	avr_irq_register_notify(axis_irq + Z_AXIS_IND, axis_irq_handler, &irq_position[Z_AXIS_IND]);
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(STEP_PORT), X_STEP_BIT), axis_irq + X_AXIS_IND);
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(STEP_PORT), Y_STEP_BIT), axis_irq + Y_AXIS_IND);
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(STEP_PORT), Z_STEP_BIT), axis_irq + Z_AXIS_IND);

	/*
	 *	VCD file initialization
	 *
	 *	This will allow you to create a "wave" file and display it in gtkwave
	 *	Pressing "r" and "s" during the demo will start and stop recording
	 *	the pin changes
	 */
//	avr_vcd_init(avr, "gtkwave_output.vcd", &vcd_file, 100000 /* usec */);
//	avr_vcd_add_signal(&vcd_file,
//		avr_io_getirq(avr, AVR_IOCTL_TIMER_GETIRQ('1'), TIMER_IRQ_OUT_COMP+AVR_TIMER_COMPA), 8 /* bits */ ,
//		"COMP1A" );
//	avr_vcd_add_signal(&vcd_file,
//		avr_io_getirq(avr, AVR_IOCTL_TIMER_GETIRQ('1'), TIMER_IRQ_OUT_COMP+AVR_TIMER_COMPB), 8 /* bits */ ,
//		"COMP1B" );
//	avr_vcd_start(&vcd_file);

	printf( "\nLaunching...\n");

	int state = cpu_Running;
	while ((state != cpu_Done) && (state != cpu_Crashed))
	{
		state = avr_run(avr);

		while(ub.xon && !uart_bridge_fifo_isempty(&ub.out)) {
			uint8_t byte = uart_bridge_fifo_read(&ub.out);
			avr_raise_irq(ub.irq + IRQ_UART_BRIDGE_BYTE_OUT, byte);
		}

		if (memcmp(sys_position_last, irq_position, sizeof(sys_position_last))) {
			if (memcmp(sys_position_m, irq_position, sizeof(sys_position_m))) {
				memcpy(sys_position_m, irq_position, sizeof(sys_position_m));
				sys_position_c = 0;
			}
			sys_position_c ++;
			if (sys_position_c == 8)
			{
				printf("%s[%u] = { %d, %d, %d }; { %d, %d, %d }; %s\n", "sys_position", N_AXIS,
					sys_position[0], sys_position[1], sys_position[2],
					irq_position[0], irq_position[1], irq_position[2],
					memcmp(sys_position, irq_position, sizeof(sys_position_last)) ? "WRONG" : "OK" );
				memcpy(sys_position_last, irq_position, sizeof(sys_position_last));
			}
		}
/*
		// CPU usage throttling
		static uint32_t step = 0;
		if(step++ < 10000UL)
			continue;
		step = 0;
		Sleep(1);
*/
	}
}
