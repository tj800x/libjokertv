/* 
 * Joker TV app
 * Supported standards:
 *
 * DVB-S/S2 – satellite, is found everywhere in the world
 * DVB-T/T2 – mostly Europe
 * DVB-C/C2 – cable, is found everywhere in the world
 * ISDB-T – Brazil, Latin America, Japan, etc
 * ATSC – USA, Canada, Mexico, South Korea, etc
 * DTMB – China, Cuba, Hong-Kong, Pakistan, etc
 *
 * (c) Abylay Ospan <aospan@jokersys.com>, 2017
 * LICENSE: GPLv2
 * https://tv.jokersys.com
 * GPLv2
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <libusb.h>

#include <queue>
#include "joker_tv.h"
#include "joker_fpga.h"
#include "joker_ci.h"
#include "joker_spi.h"
#include "joker_ts.h"
#include "u_drv_tune.h"
#include "u_drv_data.h"

// status & statistics callback
// will be called periodically after 'tune' call
void status_callback_f(void *data)
{
	struct joker_t *joker= (struct joker_t *)data;
	struct stat_t *stat = NULL;

	if (!joker)
		return;
		
	stat = &joker->stat;
	printf("INFO: status=%d (%s) ucblocks=%d, rflevel=%.3f dBm, SNR %.3f dB, BER %.2e, quality %d \n", 
			stat->status, stat->status == JOKER_LOCK ? "LOCK" : "NOLOCK",
			stat->ucblocks, (double)stat->rf_level/1000, (double)stat->snr/1000,
			(double)stat->bit_error/stat->bit_count,
			stat->signal_quality);
	fflush(stdout);

	// less heavy refresh if status locked
	if (stat->status == JOKER_LOCK)
		stat->refresh_ms = 2000;
	else
		stat->refresh_ms = 500;
}

// this callback will be called when new service name arrived
void service_name_update(struct program_t *program)
{
	struct program_es_t*es = NULL;
	printf("callback:%s program number=%d name=%s type=0x%x. video:%s audio:%s\n",
			__func__, program->number, program->name, program->service_type,
			program->has_video ? "yes" : "",
			program->has_audio ? "yes" : "");

	if(!list_empty(&program->es_list)) {
		list_for_each_entry(es, &program->es_list, list) {
			printf("	ES pid=0x%x type=0x%x\n",
					es->pid, es->type);
		}
	}
}

void show_help() {
	printf("joker-tv usage: \n");
	printf("	-d delsys	Delivery system. Options: \n \
			%d-ATSC  %d-DVB-S  %d-DVB-S2 %d-DVB-C %d-DVB-T %d-DVB-T2 %d-ISDB-T %d-DTMB\n", 
			JOKER_SYS_ATSC, JOKER_SYS_DVBS, JOKER_SYS_DVBS2, JOKER_SYS_DVBC_ANNEX_A,
			JOKER_SYS_DVBT, JOKER_SYS_DVBT2, JOKER_SYS_ISDBT, JOKER_SYS_DTMB);
	printf("	-m modulation	Modulation. Options: \n \
			%d-VSB8 (for ATSC) 0-AUTO\n", JOKER_VSB_8);
	printf("	-f freq		Frequency in Hz. Example: 1402000000\n");
	printf("	-s symbol_rate	Symbol rate. Options: 0-AUTO. Example: 20000000\n");
	printf("	-y voltage	LNB voltage. Options: 13-Vert/Right, 18-Horiz/Left, 0-OFF. Example: -y 18\n");
	printf("	-b bandwidth	Bandwidth in Hz. Example: 8000000\n");
	printf("	-o filename	Output TS filename. Default: out.ts\n");
	printf("	-t		Enable TS generator. Default: disabled\n");
	printf("	-n		Disable TS data processing. Default: enabled\n");
	printf("	-l limit	Write only limit MB(megabytes) of TS. Default: unlimited\n");
	printf("	-u level	Libusb verbose level (0 - less, 4 - more verbose). Default: 0\n");
	printf("	-w filename	Update firmware on flash. Default: none\n");
	printf("	-p		Decode programs info (DVB PSI tables). Default: no\n");
	printf("	-z l,h,s	LNB settings: low/high/switch frequency. Example: -z 9750,10600,11700\n");
	printf("	-e		Enable 22 kHz tone (continuous). Default: disabled\n");
	printf("	-c		Enable CAM module. Default: disabled\n");
	printf("	-j		Enable CAM module verbose messages. Default: disabled\n");

	exit(0);
}

int main (int argc, char **argv)
{
	struct tune_info_t info;
	struct big_pool_t pool;
	int status = 0, ret = 0, rbytes = 0, i = 0;
	struct joker_t * joker = NULL;
	unsigned char buf[JCMD_BUF_LEN];
	unsigned char in_buf[JCMD_BUF_LEN];
	int c, tsgen = 0;
	int delsys = 0, mod = 0, sr = 0, bw = 0;
	uint64_t freq = 0;
	FILE * out = NULL;
	char filename[FNAME_LEN] = "out.ts";
	char fwfilename[FNAME_LEN] = "";
	char confirm[FNAME_LEN];
	int signal = 0;
	int disable_data = 0;
	struct ts_node * node = NULL;
	unsigned char *res = NULL;
	int res_len = 0, read_once = 0;
	struct list_head *programs = NULL;
	struct program_t *program = NULL, *tmp = NULL;
	bool decode_program = false;
	int64_t total_len = 0, limit = 0;
	int voltage = 0, tone = 1;

	joker = (struct joker_t *) malloc(sizeof(struct joker_t));
	if (!joker)
		return ENOMEM;
	memset(joker, 0, sizeof(struct joker_t));
	memset(in_buf, 0, JCMD_BUF_LEN);
	memset(buf, 0, JCMD_BUF_LEN);
	memset(&pool, 0, sizeof(struct big_pool_t));

	// set callbacks
	pool.service_name_callback = &service_name_update;
	joker->status_callback = &status_callback_f;

	while ((c = getopt (argc, argv, "d:y:z:m:f:s:o:b:l:tpu:w:nhecj")) != -1)
		switch (c)
		{
			case 'd':
				delsys = atoi(optarg);
				break;
			case 'y':
				voltage = atoi(optarg);
				break;
			case 'e':
				tone = 0; /* 0 - mean tone on */
				break;
			case 'z':
				sscanf(optarg, "%d,%d,%d", &info.lnb.lowfreq, &info.lnb.highfreq, &info.lnb.switchfreq);
				break;
			case 'm':
				mod = atoi(optarg);
				break;
			case 'f':
				freq = strtoull(optarg, NULL, 10);
				break;
			case 's':
				sr = atoi(optarg);
				break;
			case 'b':
				bw = atoi(optarg);
				break;
			case 'n':
				disable_data = 1;
				break;
			case 't':
				tsgen = 1;
				break;
			case 'p':
				decode_program = 1;
				break;
			case 'u':
				joker->libusb_verbose = atoi(optarg);
				break;
			case 'c':
				joker->ci_enable = 1;
				break;
			case 'j':
				joker->ci_verbose = 1;
				break;
			case 'l':
				limit = 1024*1024*atoi(optarg);
				break;
			case 'o':
				strncpy((char*)filename, optarg, FNAME_LEN);
				break;
			case 'w':
				strncpy((char*)fwfilename, optarg, FNAME_LEN);
				break;
			case 'h':
			default:
				show_help();
		}

	out = fopen((char*)filename, "w+b");
	if (!out){
		printf("Can't open out file '%s' \n", filename);
		perror("");
		exit(-1);
	} else {
		printf("TS outfile:%s \n", filename);
	}

	/* open Joker TV on USB bus */
	if ((ret = joker_open(joker)))
		return ret;
	printf("allocated joker=%p \n", joker);

	/* init CI */
	if (joker->ci_enable)
		joker_ci(joker);

	/* upgrade fw if selected */
	if(strlen((const char*)fwfilename)) {
		if(joker_flash_checkid(joker)) {
			printf("SPI flash id check failed. Cancelling fw update.\n");
			return -1;
		}
		printf("SPI flash id check success. Please enter 'yes' to continue: ");

		if (!fgets(confirm, FNAME_LEN, stdin))
			return -1;

		if (strncmp(confirm, "yes", 3))
			return -1;

		printf("\nStarting fw update.\n");
		if(joker_flash_write(joker, fwfilename)) {
			printf("Can't write fw to flash !\n");
			return -1;
		} else {
			printf("FW successfully upgraded. Reconnect device please.\n");
			return 0;
		}
	}

	if (delsys == JOKER_SYS_UNDEFINED && tsgen !=1 )
		show_help();

	if(tsgen) {
		/* TS generator selected */
		buf[0] = J_CMD_TS_INSEL_WRITE;
		buf[1] = J_INSEL_TSGEN;
		if ((ret = joker_cmd(joker, buf, 2, NULL /* in_buf */, 0 /* in_len */)))
			return ret;
	} else {
		/* real demod selected
		 * tuning ...
		 */
		info.delivery_system = (joker_fe_delivery_system)delsys;
		info.bandwidth_hz = bw;
		info.frequency = freq;
		info.symbol_rate = sr;
		info.modulation = (joker_fe_modulation)mod;
		info.tone = (joker_fe_sec_tone_mode)tone;

		/* set LNB voltage for satellite */
		if (voltage == 13)
			info.voltage = JOKER_SEC_VOLTAGE_13;
		else if (voltage == 18)
			info.voltage = JOKER_SEC_VOLTAGE_18;
		else
			info.voltage = JOKER_SEC_VOLTAGE_OFF;

		printf("########### Tuning to %llu Hz\n", (long long)freq);
		printf("TUNE start \n");
		if (tune(joker, &info))
			return -1;
		printf("TUNE done \n");

		while (joker->stat.status != JOKER_LOCK)
			usleep(1000*100);
	}

	while(disable_data)
		sleep(3600);

	/* start TS collection */
	if((ret = start_ts(joker, &pool))) {
		printf("start_ts failed. err=%d \n", ret);
		exit(-1);
	}
	fflush(stdout);

	if (decode_program) {
		/* get TV programs list */
		printf("Trying to get programs list ... \n");
		programs = get_programs(&pool);
		list_for_each_entry_safe(program, tmp, programs, list)
			printf("Program number=%d \n", program->number);
	}

	/* get raw TS and save it to output file */
	/* reading about 18K at once */
	read_once = TS_SIZE * 100;
	res = (unsigned char*)malloc(read_once);
	if (!res)
		return -1;

	while( limit == 0 || (limit > 0 && total_len < limit) ) {
		res_len = read_ts_data(&pool, res, read_once);

		/* save to output file */
		if (res_len > 0)
			fwrite(res, res_len, 1, out);
		else
			usleep(1000); // TODO: rework this (condwait ?)

		total_len += res_len;
	}
	printf("Stopping TS ... \n");
	stop_ts(joker, &pool);

	printf("Closing device ... \n");
	joker_close(joker);
	free(joker);
}
