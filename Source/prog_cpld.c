/*
prog_cpld : very simple USB CPLD programmer
Copyright (C) 2009  Koichi Nishida

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <windows.h>
#include <stdio.h>
#include "ftd2xx.h"

// max queue length
#define USB_BUFSIZE 4096

// max string length
#define MAX_STR 256

// TAP STATES
#define TEST_LOGIC_RESET	0
#define RUN_TEST		1
#define SELECT_DR_SCAN		2
#define CAPTURE_DR		3
#define SHIFT_DR		4
#define EXIT1_DR		5
#define PAUSE_DR		6
#define EXIT2_DR		7
#define UPDATE_DR		8
#define SELECT_IR_SCAN		9
#define CAPTURE_IR		10
#define SHIFT_IR		11
#define EXIT1_IR		12
#define PAUSE_IR		13
#define EXIT2_IR		14
#define UPDATE_IR		15

// ========== global variables ==========
int g_no_match = 0;
int g_mode = 0;

// ========== prototypes ==========
// examine whether ch is blank character or not
int is_blank(int ch);

// examine whether ch is semicolon or not
int is_semi(int ch);

// get word from file FP
int get_word(FILE *fp, char *dst, int *end_semi);

// skip brace ()
int skip_brace(char *src, char *dst);

// examine whether ch is a hex character or not
int is_hex_char(int ch);

// get the value of the hex character
int value_of_hex_char(int ch);

// get the hex character of the value
int hex_char_of_value(int v);

// examine whether keyw is the keyword that can be ignored
int is_ignore(char *keyw);

// examine whether keyw is "SIR" or "SDR"
int sir_sdr(char *keyw);

// examine whether keyw is integer
int is_integer(char *keyw);

// make default mask of w bits
void make_mask(int w, char *dst);

// make zero of w bits
void make_zero(int w, char *dst);

// address TDI / TDO / SMASK / MASK
int do_param(FILE *fp, char *keyw, char* kind, char* param, int *semi);

// reset TAP machine
int reset_tap(FT_HANDLE ftHandle, int *current_state);

// wait
int wait(FT_HANDLE ftHandle, int wait_clks);

// transit state
int transit(FT_HANDLE ftHandle, int *current, int next, int wait_clks);

// output bit data
int outBit(FT_HANDLE ftHandle, int tms, int tdi, int tdo, int mask, int flush);

// output data
int outData(FT_HANDLE ftHandle, int bitw, char *tdi, char *smask, char *tdo, char *mask);

// get state from state name
int state_of_string(char *n, int *s);

// parse SVF file
int parse_svf(FILE *fp, FT_HANDLE ftHandle, int v, int *current_state);

// ========== functions ==========
// examine whether ch is blank character or not
int is_blank(int ch)
{
	return (ch == ' ' ||  ch == '\t');
}

// examine whether ch is semicolon or not
int is_semi(int ch)
{
	return (ch == ';');
}

int is_lf(int ch)
{
	return (ch == '\n' || ch == '\r');
}

// get a word from FILE* fp
// return 1 if success
int get_word(FILE *fp, char *dst, int *end_semi)
{
	static char ch = 0;
	int semi = 0;

	// pre read
	if (!ch) ch = fgetc(fp);
SCAN_START:
	if (feof(fp)) return 0;
	// skip blank
	while (is_blank(ch) || is_lf(ch)) {
		if (feof(fp)) return 0;
		ch = fgetc(fp);
	}
	// skip comment
	if (ch == '/') {
		ch = fgetc(fp);
		if (ch == '/') {
			while (!is_lf(ch)) {
				if (feof(fp)) return 0;
				ch = fgetc(fp);
			};
			goto SCAN_START;
		} else *(dst++) = '/';
	}
	// actual read
	while (!(is_blank(ch) || is_semi(ch))) {
		if(!is_lf(ch)) *(dst++) = ch;
		if (feof(fp)) break;
		ch = fgetc(fp);
	}
	*dst = 0;
	// skip blank again
	while (is_blank(ch) || is_semi(ch) || is_lf(ch)) {
		if (!semi && is_semi(ch)) semi = 1;
		if (feof(fp)) break;
		ch = fgetc(fp);
	}
	*end_semi = semi;
	return 1;
}

// skip brace ()
int skip_brace(char *src, char *dst)
{
	if (*(src++) != '(') return 0;
	while (*src != ')') { *(dst++) = *(src++); }
	*dst = 0;
	return 1;
}

// examine whether ch is a hex character or not
int is_hex_char(int ch)
{
	return ('0' <= ch && ch <= '9') || ('a' <= ch && ch <= 'f');
}

// get the value of the hex character
int value_of_hex_char(int ch)
{
	if ('0' <= ch && ch <= '9') return ch - '0';
	return (ch - 'a' + 10);
}

// get the hex character of the value
int hex_char_of_value(int v)
{
	if (v <= 9) return v + '0';
	return (v - 10 + 'a');
}

// examine whether keyw is ignore keyword or not
int is_ignore(char *keyw)
{
	int i;
	const char ignores[6][10] = {"HIR", "TIR", "HDR", "TDR", "TRST", "FREQUENCY"};

	for (i = 0; i < 6; i++) {
		if (!strcmp(ignores[i], keyw)) return 1;
	}
	return 0;
}

// examine whether keyw is SIR or SDR
int sir_sdr(char *keyw)
{
	int i;
	const char coms[2][4] = {"SIR", "SDR"};

	for (i = 0; i < 2; i++)
		if (!strcmp(coms[i], keyw)) return 1;
	return 0;
}

// examine whether kew is integer
int is_integer(char *keyw)
{
	const char *src = keyw;
	int c;
	while (c = *src++) { if (!isdigit(c)) return 0; }
	return 1;
}

// make default mask of w bits
void make_mask(int w, char *dst)
{
	int i, d = 0, n = w / 4, m = w % 4, len = (w+3)/4;

	while (m-- > 0) d = d*2+1;
	if (d) *(dst++) = hex_char_of_value(d);
	for (i = 0; i < n; i++) *(dst++) = 'f';
	*dst = 0;
}

// make zero of w bits
void make_zero(int w, char *dst)
{
	int i, len = (w+3)/4;

	for (i=0; i<len; i++) *(dst++) = '0';
	*dst = 0;
}

// address TDI / TDO / SMASK / MASK
int do_param(FILE *fp, char *keyw, char* kind, char* param, int *semi)
{
	char buff[MAX_STR];
	if (!strcmp(keyw, kind)) {
		if (!get_word(fp, buff, semi)) return 0;
		skip_brace(buff, param);
	}
	return 1;
}

// output bit data
int outBit(FT_HANDLE ftHandle, int tms, int tdi, int tdo, int mask, int flush)
{
	static int length = 0, explength[2]={0,0}, first = 1, index = 0;
	static unsigned char buff[USB_BUFSIZE], expect[USB_BUFSIZE/2][2], result[USB_BUFSIZE];
	DWORD written, read;
	int i, j;

	buff[length++] = ((tms << 1) | tdi);
	buff[length++] = (4 | (tms << 1) | tdi);
	if (g_mode == 1) expect[explength[index]++][index] = (mask ? (tdo | 2) : 0);

	// write read to/from USB
	if ((length == USB_BUFSIZE) || flush) {
		FT_Write(ftHandle, buff, length, &written);
		if (written != length) return 0;
		length = 0;
		if (g_mode == 1) {
			if (!first) {
				for (j=0; j<(flush+1); j++) {
					int prev_idx = (index?0:1);
					FT_Read(ftHandle, result, explength[prev_idx]*2, &read);
					if (read != (explength[prev_idx]*2)) return 0;
					for (i=0; i<explength[prev_idx]; i++) {
						int exp = (expect[i][prev_idx]&1);
						int msk = (expect[i][prev_idx]&2);
						int res = ((result[i*2+1]&8)?1:0);
						if (msk) {
							if (exp != res) { g_no_match++; }
						}
					}
					explength[prev_idx] = 0;
					index = (index?0:1);
				}
				if (flush) first = 1;
			} else {
				index = (index?0:1);
				first = 0;
			}
		}
	}
	return 1;
}

// output data
int outData(FT_HANDLE ftHandle, int bitw, char *tdi, char *smask, char *tdo, char *mask)
{
	int i, tdi_, smask_, tdo_, mask_;

	while (*(tdi++)) ; tdi -= 2;
	while (*(smask++)) ; smask -= 2;
	while (*(tdo++)) ; tdo -= 2;
	while (*(mask++)) ; mask -= 2;
	for (i = 0; i < bitw; i++) {
		if ((i % 4) == 0) {
			tdi_ = value_of_hex_char(*(tdi--));
			smask_ = value_of_hex_char(*(smask--));
			tdo_ = value_of_hex_char(*(tdo--));
			mask_ = value_of_hex_char(*(mask--));
		}
		if (!outBit(ftHandle, (i == (bitw-1)) ? 1 : 0, (tdi_&1), (tdo_&1), (mask_&1), 0)) {
			return 0;
		}
		tdi_ >>= 1;
		smask_ >>= 1;
		tdo_ >>= 1;
		mask_ >>= 1;
	}
	return 1;
}

// reset TAP machine
int reset_tap(FT_HANDLE ftHandle, int *current_state)
{
	int i;
	for (i = 0; i < 5; i++) if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
	*current_state = TEST_LOGIC_RESET;
	return 1;
}

int wait(FT_HANDLE ftHandle, int wait_clks) {
	int i;
	for (i = 0; i < wait_clks; i++) if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
	return 1;
}

// transit state
int transit(FT_HANDLE ftHandle, int *current, int next, int wait_clks)
{
	if (*current == next) {
		if (!wait(ftHandle, wait_clks)) return 0;
		return 1;
	}
	switch (*current) {
	case TEST_LOGIC_RESET:
		if (next != RUN_TEST) return 0;
		if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
		*current = RUN_TEST;
		break;
	case RUN_TEST:
		if (next == SELECT_DR_SCAN) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = SELECT_DR_SCAN;
		}
		else if (next == PAUSE_DR) {
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_DR, 0)) return 0;
			if (!transit(ftHandle, current, EXIT1_DR, 0)) return 0;
			if (!transit(ftHandle, current, PAUSE_DR, 0)) return 0;
		} else if (next == PAUSE_IR) {
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, SELECT_IR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_IR, 0)) return 0;
			if (!transit(ftHandle, current, EXIT1_IR, 0)) return 0;
			if (!transit(ftHandle, current, PAUSE_IR, 0)) return 0;
		} else if (next == SHIFT_DR) {
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_DR, 0)) return 0;
			if (!transit(ftHandle, current, SHIFT_DR, 0)) return 0;
		} else if (next == SHIFT_IR) {
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, SELECT_IR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_IR, 0)) return 0;
			if (!transit(ftHandle, current, SHIFT_IR, 0)) return 0;
		} else { return 0; }
		break;
	case SELECT_DR_SCAN:
		if (next == CAPTURE_DR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = CAPTURE_DR;
		} else if (next == SELECT_IR_SCAN) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = SELECT_IR_SCAN;
		} else { return 0; }
		break;
	case CAPTURE_DR:
		if (next == SHIFT_DR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = SHIFT_DR;
		} else if (next == EXIT1_DR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = EXIT1_DR;
		} else { return 0; }
		break;
	case SHIFT_DR:
		if (next != EXIT1_DR) return 0;
		if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
		*current = EXIT1_DR;
		break;
	case EXIT1_DR:
		if (next == PAUSE_DR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = PAUSE_DR;
		} else if (next == UPDATE_DR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = UPDATE_DR;
		} else if (next == RUN_TEST) {
			if (!transit(ftHandle, current, UPDATE_DR, 0)) return 0;
			if (!transit(ftHandle, current, RUN_TEST, 0)) return 0;
		} else { return 0; }
		break;
	case PAUSE_DR:
		if (next == EXIT2_DR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = EXIT2_DR;
		}
		else if (next == RUN_TEST) {
			if (!transit(ftHandle, current, EXIT2_DR, 0)) return 0;
			if (!transit(ftHandle, current, UPDATE_DR, 0)) return 0;
			if (!transit(ftHandle, current, RUN_TEST, 0)) return 0;
		} else if (next == SHIFT_IR) {
			if (!transit(ftHandle, current, EXIT2_DR, 0)) return 0;
			if (!transit(ftHandle, current, UPDATE_DR, 0)) return 0;
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, SELECT_IR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_IR, 0)) return 0;
			if (!transit(ftHandle, current, SHIFT_IR, 0)) return 0;
		} else { return 0; }
		break;
	case EXIT2_DR:
		if (next == SHIFT_DR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = SHIFT_DR;
		} else if (next == UPDATE_DR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = UPDATE_DR;
		} else { return 0; }
		break;
	case UPDATE_DR:
		if (next == RUN_TEST) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = RUN_TEST;
		} else if (next == SELECT_DR_SCAN) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = SELECT_DR_SCAN;
		} else { return 0; }
		break;
	case SELECT_IR_SCAN:
		if (next == CAPTURE_IR ) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = CAPTURE_IR;
		} else if (next == TEST_LOGIC_RESET) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = TEST_LOGIC_RESET;
		} else { return 0; }
		break;
	case CAPTURE_IR:
		if (next == SHIFT_IR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = SHIFT_IR;
		} else if (next == EXIT1_IR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = EXIT1_IR;
		} else { return 0; }
		break;
	case SHIFT_IR:
		if (next != EXIT1_IR) return 0;
		if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
		*current = EXIT1_IR;
		break;
	case EXIT1_IR:
		if (next == PAUSE_IR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = PAUSE_IR;
		} else if (next == UPDATE_IR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = UPDATE_IR;
		} else if (next == RUN_TEST) {
			if (!transit(ftHandle, current, UPDATE_IR, 0)) return 0;
			if (!transit(ftHandle, current, RUN_TEST, 0)) return 0;
		} else { return 0; }
		break;
	case PAUSE_IR:
		if (next == EXIT2_IR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = EXIT2_IR;
		} else if (next == RUN_TEST) {
			if (!transit(ftHandle, current, EXIT2_IR, 0)) return 0;
			if (!transit(ftHandle, current, UPDATE_IR, 0)) return 0;
			if (!transit(ftHandle, current, RUN_TEST, 0)) return 0;
		} else if (next == SHIFT_DR) {
			if (!transit(ftHandle, current, EXIT2_IR, 0)) return 0;
			if (!transit(ftHandle, current, UPDATE_IR, 0)) return 0;
			if (!transit(ftHandle, current, SELECT_DR_SCAN, 0)) return 0;
			if (!transit(ftHandle, current, CAPTURE_DR, 0)) return 0;
			if (!transit(ftHandle, current, SHIFT_DR, 0)) return 0;
		} else { return 0; }
		break;
	case EXIT2_IR:
		if (next == SHIFT_IR) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = SHIFT_IR;
		} else if (next == UPDATE_IR) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = UPDATE_IR;
		} else { return 0; }
		break;
	case UPDATE_IR:
		if (next == RUN_TEST) {
			if (!outBit(ftHandle, 0, 0, 0, 0, 0)) return 0;
			*current = RUN_TEST;
		} else if (next == SELECT_DR_SCAN) {
			if (!outBit(ftHandle, 1, 0, 0, 0, 0)) return 0;
			*current = SELECT_DR_SCAN;
		} else { return 0; }
		break;
	}
	if (!wait(ftHandle, wait_clks)) return 0;
	return 1;
}

// get state from state name
int state_of_string(char *n, int *s)
{
	if (!strcmp(n, "IDLE")) *s = RUN_TEST;
	else if (!strcmp(n, "RESET")) *s = TEST_LOGIC_RESET;
	else if (!strcmp(n, "IRPAUSE")) *s = PAUSE_IR;
	else if (!strcmp(n, "DRPAUSE")) *s = PAUSE_DR;
	else if (!strcmp(n, "IREXIT1")) *s = EXIT1_IR;
	else if (!strcmp(n, "IREXIT2")) *s = EXIT2_IR;
	else if (!strcmp(n, "DREXIT1")) *s = EXIT1_DR;
	else if (!strcmp(n, "DREXIT2")) *s = EXIT2_DR;
	else if (!strcmp(n, "IRUPDATE")) *s = UPDATE_IR;
	else if (!strcmp(n, "DRUPDATE")) *s = UPDATE_DR;
	else if (!strcmp(n, "IRSELECT")) *s = SELECT_IR_SCAN;
	else if (!strcmp(n, "DRSELECT")) *s = SELECT_DR_SCAN;
	else if (!strcmp(n, "IRCAPTURE")) *s = CAPTURE_IR;
	else if (!strcmp(n, "DRCAPTURE")) *s = CAPTURE_DR;
	else {
		fprintf(stderr, "unknown state(%s)\n", n);
		return 0;
	}
	return 1;
}

// parse SVF file
int parse_svf(FILE *fp, FT_HANDLE ftHandle, int v, int *current_state)
{
	char keyw[MAX_STR], keyw2[MAX_STR], tdi[MAX_STR], tdo[MAX_STR];
	static char smask_sir[MAX_STR], mask_sir[MAX_STR];
	static char smask_sdr[MAX_STR], mask_sdr[MAX_STR];
	static char mask_tmp[MAX_STR];
	char *smask, *mask;
	int semi, ret, bitw, clks;
	int end_ir = RUN_TEST, end_dr = RUN_TEST, run_state = RUN_TEST;

	while (get_word(fp, keyw, &semi)) {
		if (is_ignore(keyw)) {
			do {
				ret = get_word(fp, keyw, &semi);
			} while (ret && !semi);
		} else if (sir_sdr(keyw)) {
			int next_state;

			if (!strcmp(keyw, "SIR")) {
				next_state = SHIFT_IR;
				mask = mask_sir;
				smask = smask_sir;
			} else {
				next_state = SHIFT_DR;
				mask = mask_sdr;
				smask = smask_sdr;
			}
			if (!transit(ftHandle, current_state, next_state, 0)) {
				return 1;
			}
			if (!get_word(fp, keyw2, &semi)) return 2;
			if (!is_integer(keyw2)) return 3;
			bitw = atoi(keyw2);
			*tdi = *tdo = 0;
			do {
				if (!get_word(fp, keyw2, &semi)) return 4;
				if (!do_param(fp, keyw2, "TDI", tdi, &semi)) return 5;
				if (!do_param(fp, keyw2, "TDO", tdo, &semi)) return 6;
				if (!do_param(fp, keyw2, "SMASK", smask, &semi)) return 7;
				if (!do_param(fp, keyw2, "MASK", mask, &semi)) return 8;
			} while (!semi);
			if (*tdo == 0) {
				make_zero(bitw, tdo);
				mask = mask_tmp;
				make_zero(bitw, mask);
			}
			if (v) printf("%s %d TDI %s SMASK %s TDO %s MASK %s\n", keyw, bitw, tdi, smask, tdo, mask);
			if (!outData(ftHandle, bitw, tdi, smask, tdo, mask)) {
				return 9;
			}
			if (!strcmp(keyw, "SIR")) {
				*current_state = EXIT1_IR;
				if (!transit(ftHandle, current_state, end_ir, 0)) return 10;
			} else {
				*current_state = EXIT1_DR;
				if (!transit(ftHandle, current_state, end_dr, 0)) return 11;
			}
		} else if (!strcmp(keyw, "RUNTEST")) {
			if (!get_word(fp, keyw, &semi)) return 12;
			if (is_integer(keyw)) { clks = atoi(keyw); }
			else {
				int n;
				if (!state_of_string(keyw, &n)) return 13;
				run_state = n; 
				if (!get_word(fp, keyw, &semi)) return 14;
				if (!is_integer(keyw)) return 15;
				clks = atoi(keyw);
			}
			if (!get_word(fp, keyw, &semi)) return 16;
			if (strcmp(keyw, "TCK") || !semi) { // not supported format
				return 17;
			}
			if (v) printf("RUNTEST %d TCK\n", clks); fflush(stdout);
			if (!transit(ftHandle, current_state, run_state, clks)) return 18; // end_state = run_state
		} else if (!strcmp(keyw, "STATE")) {
			if (v) printf("STATE ");
			do {
				int n;
				if (!get_word(fp, keyw2, &semi)) return 19;
				if (!state_of_string(keyw2, &n)) return 20;
				if (!transit(ftHandle, current_state, n, 0)) return 21;
				if (v) printf("%s ", keyw2);
			} while (!semi);
			if (v) printf("\n");
		} else if (!strcmp(keyw, "ENDIR")) {
			int n;
			if (!get_word(fp, keyw2, &semi)) return 22;
			if (v) printf("ENDIR %s\n", keyw2);
			if (!state_of_string(keyw2, &n)) return 23;
			end_ir = n;
		} else if (!strcmp(keyw, "ENDDR")) {
			int n;
			if (!get_word(fp, keyw2, &semi)) return 24;
			if (v) printf("ENDDR %s\n", keyw2);
			if (!state_of_string(keyw2, &n)) return 25;
			end_dr = n;
		} else return 26;
	}
	return 0;
}

int main(int argc, char* argv[])
{
	FT_HANDLE ftHandle;
	FT_STATUS ftStatus;
	FILE *fp;
	char *arg, *fname = NULL;
	int i, v = 0;
	int current_state;
	int error_code = 0;
	errno_t errno;

	// initialize global variables
	g_no_match = 0;
	g_mode = 0;

	for (i = 1 ; i < argc; i++) {
		arg = argv[i];
		if (!strcmp(arg, "-v")) v = 1;
		else if (!strcmp(arg, "-c")) g_mode = 1;
		else if (!strcmp(arg, "-h")) {
			printf("prog_cpld svf_file [options]\n");
			printf(" options:\n");
			printf("   -c compare TDO outputs to the expected values\n");
			printf("   -v verbose\n");
			printf("   -h help\n");
			return 0;
		} else fname = arg;
	}
	if (!fname) {
		fprintf(stderr, "speciry SVF file\n");
		return 0;
	}
	if (errno = fopen_s(&fp, fname, "r")) {
		fprintf(stderr, "can't open %s(%d)\n",  fname, errno);
		return 0;
	}

	{ // dummy open...
		ftStatus = FT_Open(0, &ftHandle);
		ftStatus = FT_SetBitMode(ftHandle, 7, (g_mode == 1)?4:1);
		FT_Close(ftHandle);
	}
//	ftStatus = FT_OpenEx("JTAG", FT_OPEN_BY_DESCRIPTION, &ftHandle);
	ftStatus = FT_Open(0, &ftHandle);
	if (ftStatus != FT_OK) {
		fprintf(stderr, "can't open USB device\n");
		goto ERROR2;
	}
	// synchronous or asynchronous bit bang mode
	// (d3,d2,d1,d0) = (TDO,TCK,DMS,TDI)
	ftStatus = FT_SetBitMode(ftHandle, 7, (g_mode == 1)?4:1);
	if (ftStatus != FT_OK) {
		fprintf(stderr, "can't initialize USB device\n");
		goto ERROR1;
	}

	FT_SetDivisor(ftHandle, 1);
	if (!reset_tap(ftHandle, &current_state)) {
		fprintf(stderr, "can't write to USB\n");
		goto ERROR1;
	}
	if (error_code = parse_svf(fp, ftHandle, v, &current_state)) {
		fprintf(stderr, "parse error(errorcode = %d)\n", error_code);
		goto ERROR1;
	}
	// flush USB
	outBit(ftHandle, 0, 0, 0, 0, 1);
	if (g_mode == 1) {
		if (g_no_match > 0) printf("\n   <<< %d TDO outputs didn't match to the expected values... >>>\n\n", g_no_match);
		else printf("\n   <<< All TDO outputs matched to the expected values! >>>\n\n");
	}
ERROR1:
	FT_Close(ftHandle);
ERROR2:
	fclose(fp);
	fflush(stderr);

	return 0;
}
