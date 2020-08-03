/*
 * Copyright 2020 Hans Chinghan Yu <chinghanyu@ucsd.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * See COPYING for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <argp.h>
#include <endian.h>
#include <math.h>

//#include <arpa/inet.h>

//#include <net/ethernet.h>

//#include <netinet/in.h>

#include <sys/eventfd.h>

#include <moep80211/system.h>
#include <moep80211/types.h>
//#include <moep80211/ieee80211_addr.h>

#include <moep80211/modules/ieee80211.h>
#include <moep80211/modules/moep80211.h>

#include "../src/util.h"

static char args_doc[] = "IF1 IF2";

static char doc[] =
		"wakeupgen - a wake-up pattern generator\n\n"
		"  IF1                        Radio interface number 1\n"
		"  IF2                        Radio interface number 2";

enum fix_args {
	FIX_ARG_IF1 = 0,
	FIX_ARG_IF2 = 1,
	FIX_ARG_FREQ = 2,
	FIX_ARG_D1 = 3,
	FIX_ARG_D2 = 4,
	FIX_ARG_D3 = 5,
	FIX_ARG_D4 = 6,
	FIX_ARG_CNT
};

static struct argp_option options[] = {
		{}
};

static error_t parse_opt(int key, char *arg, struct argp_state *state);

static struct argp argp = {
		options,
		parse_opt,
		args_doc,
		doc
};

struct arguments {
	char *if1;
	char *if2;
	u64 freq;
	int d1;
	int d2;
	int d3;
	int d4;
};

#define DEFAULT_MTU 500
#define DEFAULT_CHAN chan_freq[0]
#define IEEE80211G_PREAMBLE_DURATION_US 20
#define IEEE80211G_SYMBOL_DURATION_US 4
#define MAC_HEADER_LEN 28
#define MAX_PAYLOAD_LEN 2346 - MAC_HEADER_LEN

static u64 chan_freq[] = {
		2412,
		2417,
		2422,
		2427,
		2432,
		2437,
		2442,
		2447,
		2452,
		2457,
		2462,
		2467,
		2472,
		2484,

		5180,
		5200,
		5220,
		5240,
		5260,
		5280,
		5300,
		5320,

		5500,
		5520,
		5540,
		5560,
		5580,
		5600,
		5620,
		5640,
		5660,
		5680,
		5700,

		5735,
		5755,
		5775,
		5795,
		5815,
		5835,
		5855,
};

/*
 * Numbers are doubled due to decimal point 5.5. Valid legacy rates should be:
 *   1, 2, 5.5, 6, 9, 11, 12, 18, 24, 36, 48, and 54 Mbps
 *   | <--- 80.11b ---> | <---     802.11g     ---> |
 */
int legacy_rates[] = {
		2, 4, 11, 12, 18, 22, 24, 36, 48, 72, 96, 108
};

int legacy_11g_2x_rates[] = {
		12, 18, 24, 36, 48, 72, 96, 108
};

static int legacy_idx(int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(legacy_rates); i++) {
		if (legacy_rates[i] == rate)
			return i;
	}
	return -1;
}

/* We need to solve that given a specific air time duration, what is the least achievable rate and payload length */
struct pkt_param {
	int rate_idx;
	int payload_len;
	int valid_duration;
};

#define OFFSET	-6
static struct pkt_param find_radio_param(unsigned int duration_us) {
	struct pkt_param param = {-1, 0, 0};
	int i, k, payload_len;
	// specified duration has to be at least the preamble duration, 20 us in 11g
	if (duration_us < IEEE80211G_PREAMBLE_DURATION_US)
		return param;

	k = ceil((duration_us - IEEE80211G_PREAMBLE_DURATION_US) / (double) IEEE80211G_SYMBOL_DURATION_US);
	param.valid_duration = IEEE80211G_PREAMBLE_DURATION_US + k * IEEE80211G_SYMBOL_DURATION_US;

	for (i = 0; i < ARRAY_SIZE(legacy_11g_2x_rates); i++) {
		// payload_len = symbol_duration * k * rate / 8 - MAC_HEADER_LEN + OFFSET
		payload_len = IEEE80211G_SYMBOL_DURATION_US * k * legacy_11g_2x_rates[i] / 2 / 8 - MAC_HEADER_LEN + OFFSET;
		if ((payload_len >= 0) && (payload_len <= MAX_PAYLOAD_LEN)) {
			param.rate_idx = i;
			param.payload_len = payload_len;
			break;
		}
	}

	return param;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	struct arguments *args = state->input;

	switch (key) {
		case ARGP_KEY_ARG:
			switch (state->arg_num) {
				case FIX_ARG_IF1:
					args->if1 = arg;
					break;
				case FIX_ARG_IF2:
					args->if2 = arg;
					break;
				case FIX_ARG_FREQ:
					args->freq = strtol(arg, NULL, 10);
					break;
				case FIX_ARG_D1:
					args->d1 = (int) strtol(arg, NULL, 10);
					break;
				case FIX_ARG_D2:
					args->d2 = (int) strtol(arg, NULL, 10);
					break;
				case FIX_ARG_D3:
					args->d3 = (int) strtol(arg, NULL, 10);
					break;
				case FIX_ARG_D4:
					args->d4 = (int) strtol(arg, NULL, 10);
					break;
				default:
					argp_usage(state);
			}
			break;
		case ARGP_KEY_END:
			if (state->arg_num < FIX_ARG_CNT)
				argp_usage(state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int dev_avail(const char *name) {
	moep_dev_t dev;

	if (!(dev = moep_dev_moep80211_open(name, DEFAULT_CHAN,
										MOEP80211_CHAN_WIDTH_20_NOHT, 0, 0,
										DEFAULT_MTU))) {
		fprintf(stderr, "cannot create device on '%s': %s\n", name, strerror(errno));
		return -1;
	}
	moep_dev_close(dev);

	return 0;
}

static int avail_channels(const char *name) {
	moep_dev_t dev;
	int i;
	u64 chan;

	chan = 0;
	printf("available channels %s: ", name);
	for (i = 0; i < ARRAY_SIZE(chan_freq); i++) {
		if (!(dev = moep_dev_moep80211_open(name, chan_freq[i],
											MOEP80211_CHAN_WIDTH_20_NOHT,
											0, 0, DEFAULT_MTU))) {
			printf("-");
			fflush(stdout);
			continue;
		}
		moep_dev_close(dev);
		chan |= BIT(i);
		printf("+");
		fflush(stdout);
	}
	printf("\n");

	return chan;
}

static u8 data_rate_work_payload[] = "Hello, world!";	// 12 bytes

struct moep_hdr_pctrl data_rate_hdr_pctrl = {
		.hdr = {
				.type = MOEP_HDR_PCTRL,							// 0x01
				.len = sizeof(struct moep_hdr_pctrl),			// (8 + 8 + 16 + 16) = 48 bits = 6 bytes = 0x06
		},
		.type = htole16(0),									// 0x00, 0x00
		.len = htole16(sizeof(data_rate_work_payload)),		// 12 bytes = 0x0c, 0x00, need to be updated
};

static unsigned char *gen_payload(size_t num_bytes) {
	// Don't forget to set the random seed.
	unsigned char *payload = malloc(num_bytes);
	size_t i;

	for (i = 0; i < num_bytes; i++)
		payload[i] = 'A';

	return payload;
}

static int print_payload(unsigned char *payload, int len) {
	int i;
	printf("Payload: ");
	for (i = 0; i < len; i++)
		printf("%x", payload[i]);

	printf("\n");
	return 0;
}

static u16 legacy_rate_worked;
static u32 mcs_rate_worked;

static void rx_handler_work(moep_dev_t dev, moep_frame_t frame)
{
	struct moep80211_radiotap *radiotap;
	struct moep80211_hdr *hdr;
	struct moep_hdr_pctrl *pctrl;

	if (!(hdr = moep_frame_moep80211_hdr(frame))) {
		moep_frame_destroy(frame);
		return;
	}
	if (hdr->frame_control !=
		htole16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA)) {
		moep_frame_destroy(frame);
		return;
	}

	if (!(pctrl = (struct moep_hdr_pctrl *)moep_frame_moep_hdr_ext(frame, MOEP_HDR_PCTRL))) {
		moep_frame_destroy(frame);
		return;
	}
	if (pctrl->hdr.len != sizeof(struct moep_hdr_pctrl)) {
		moep_frame_destroy(frame);
		return;
	}
	if (pctrl->len != htole16(sizeof(data_rate_work_payload))) {
		moep_frame_destroy(frame);
		return;
	}
	if (pctrl->type != htole16(0)) {
		moep_frame_destroy(frame);
		return;
	}

	// TODO validate payload

	if (!(radiotap = moep_frame_radiotap(frame))) {
		moep_frame_destroy(frame);
		return;
	}
	if (radiotap->hdr.it_present & BIT(IEEE80211_RADIOTAP_RATE))
		legacy_rate_worked |= BIT(legacy_idx(radiotap->rate));
	if ((radiotap->hdr.it_present & BIT(IEEE80211_RADIOTAP_MCS)) &&
		(radiotap->mcs.known & IEEE80211_RADIOTAP_MCS_HAVE_MCS))
		mcs_rate_worked |= BIT(radiotap->mcs.mcs);

	moep_frame_destroy(frame);
}

static void test_data_rates_work(const char *send_if, const char *recv_if, u64 freq,
								 struct pkt_param pkt_params[], const int num) {
	/* Test if a frame with specified data rate in its radiotap header is correctly
	 * sent by one interface and received by the other.
	 */
	moep_dev_t moep80211_dev1 = NULL, moep80211_dev2 = NULL, ieee80211_dev1 = NULL;
	int i, k;
	struct timespec timeout;
	moep_frame_t frames[num];
	struct moep80211_radiotap *radiotap;
	struct moep80211_hdr *hdr;
	int event_all = 0;
	unsigned char *payload;

	printf("%s -> %s, %ld MHz: ", send_if, recv_if, freq);

	/* Create monitor interfaces for injection and verification. See moep80211.c and radio.c for
	 * more information. Note that if a device is opened as moep80211 device, it can only be used to sent
	 * moep frames; if we want to send standard 802.11 frame, we need to use moep_dev_ieee80211_open().
	 *
	 * For now we only do not really care about if RTS/CTS/ACK are received so we need only the sending
	 * device.
	 */
	if (!(moep80211_dev1 = moep_dev_moep80211_open(send_if, freq,
												   MOEP80211_CHAN_WIDTH_20_NOHT, 0, 0,
												   DEFAULT_MTU))) {
		fprintf(stderr, "cannot create moep80211 device on %s, %s\n", send_if, strerror(errno));
		goto out;
	}
	if (!(moep80211_dev2 = moep_dev_moep80211_open(recv_if, freq,
												   MOEP80211_CHAN_WIDTH_20_NOHT, 0, 0,
												   DEFAULT_MTU))) {
		fprintf(stderr, "cannot create device on %s, %s\n", recv_if, strerror(errno));
		goto out;
	}

	// Set recv_if to be the receiving device. Attach an RX handler onto it.
	moep_dev_set_rx_handler(moep80211_dev2, rx_handler_work);
	// Event filter descriptor?
	if ((event_all = eventfd(1, EFD_NONBLOCK | EFD_SEMAPHORE)) < 0) {
		fprintf(stderr, "cannot create eventfd, %s\n", strerror(errno));
		goto out;
	}
	// Subscribe to the events
	moep_dev_set_rx_event(moep80211_dev2, event_all);

	/* Create an moep frame and set it to be sent through device 1
	 * Refer to dev.c and frame.c for more information. Note that moep frame is not a standard 802.11 frame.
	 */
	for (i = 0; i < num; i++) {
		frames[i] = NULL;
		if (!(frames[i] = moep_dev_frame_create(moep80211_dev1))) {
			fprintf(stderr, "cannot create frame %d, %s\n", i, strerror(errno));
			goto out;
		}

		if (!(radiotap = moep_frame_radiotap(frames[i])))
			goto out;
		radiotap->hdr.it_version = 0;
		/* Set data rate of the moep frame */
		radiotap->rate = legacy_11g_2x_rates[pkt_params[i].rate_idx];

		/* Set to use the short preamble */
		radiotap->flags = IEEE80211_RADIOTAP_F_SHORTPRE;

		/* Hans: it looks like NOACK and RTS flag are effective but CTS is not */
		radiotap->tx_flags = IEEE80211_RADIOTAP_F_TX_CTS
							 | IEEE80211_RADIOTAP_F_TX_NOACK;

		radiotap->hdr.it_present = BIT(IEEE80211_RADIOTAP_RATE)
								   | BIT(IEEE80211_RADIOTAP_FLAGS)
								   | BIT(IEEE80211_RADIOTAP_TX_FLAGS);

		// Make the created frame an moep80211 frame and extract its header, see moep80211.c for more information
		// If we want to create RTS or CTS frame, we cannot use this function
		if (!(hdr = moep_frame_moep80211_hdr(frames[i])))
			goto out;
		hdr->frame_control = htole16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA);
		/* Set source and destination MAC addresses so that we can tell which is which */
		// TODO: find a way to set hardware MAC address to the packet
		memcpy(hdr->ra, "\xde\xad\xbe\xef\x02\x01", sizeof(hdr->ra));
		memcpy(hdr->ta, "\xde\xad\xbe\xef\x02\x02", sizeof(hdr->ta));

		if (!moep_frame_set_moep_hdr_ext(frames[i], &data_rate_hdr_pctrl.hdr))
			goto out;

		// Generate a payload of specified length
		payload = gen_payload(pkt_params[i].payload_len);

		if (!moep_frame_set_payload(frames[i], payload, pkt_params[i].payload_len))
			goto out;
	}

	for (k = 0; k < 1; k++) {
		for (i = 0; i < 4; i++) {
			if (moep_dev_tx(moep80211_dev1, frames[i])) {
				printf("transmitting packet %d with payload length %u at data rate %.2f "
					   "MBit/s failed: %s\n",
					   i,
					   pkt_params[i].payload_len,
					   (float) legacy_rates[i] / 2,
					   strerror(errno));
			}
		}
	}

	timeout.tv_sec = 1;
	timeout.tv_nsec = 0;

	legacy_rate_worked = 0;
	mcs_rate_worked = 0;

	moep_select(0, NULL, NULL, NULL, &timeout, NULL);

	for (i = 0; i < ARRAY_SIZE(legacy_rates); i++) {
		if (legacy_rate_worked & BIT(i)) {
			printf("+");
		} else {
			printf("-");
		}
	}
	printf("|");
	for (i = 0; i < 32; i++) {
		if (mcs_rate_worked & BIT(i)) {
			printf("+");
		} else {
			printf("-");
		}
	}
	printf("\n");

	out:
	for (i = 0; i < num; i++) {
		if (frames[i])
			moep_frame_destroy(frames[i]);
	}
	if (ieee80211_dev1)
		moep_dev_close(ieee80211_dev1);
	if (moep80211_dev1)
		moep_dev_close(moep80211_dev1);
	if (moep80211_dev2)
		moep_dev_close(moep80211_dev2);

	close(event_all);
}

int main(int argc, char **argv) {
	struct arguments args;
	struct pkt_param pkt_params[4];
	u64 chan1, chan2;
	int i;

	memset(&args, 0, sizeof(args));
	argp_parse(&argp, argc, argv, 0, 0, &args);

	// Test if the interfaces are available, if not, try rfkill unblock all
	if (dev_avail(args.if1))
		return -1;
	if (dev_avail(args.if2))
		return -1;

	/*
	// No need to test the maximum MTU here.
	printf("Computing maximum MTU...\n");
	max_mtu(args.if1);
	max_mtu(args.if2);
	*/

	// I guess there is no need to test channels, either.
	printf("Testing available channels...\n");
	chan1 = avail_channels(args.if1);
	chan2 = avail_channels(args.if2);

	// Assume all 11b and 11g rates are supported.

	// Print assigned durations
	printf("Assigned durations are %u, %u, %u, and %u\n", args.d1, args.d2, args.d3, args.d4);
	pkt_params[0] = find_radio_param(args.d1);
	pkt_params[1] = find_radio_param(args.d2);
	pkt_params[2] = find_radio_param(args.d3);
	pkt_params[3] = find_radio_param(args.d4);

	for (i = 0; i < 4; i++) {
		printf("Duration %d is achievable with rate %.1f Mbps, "
			   "payload length %d byte(s), and valid duration %d us.\n",
			   i + 1,
			   legacy_11g_2x_rates[pkt_params[i].rate_idx] / 2.0,
			   pkt_params[i].payload_len,
			   pkt_params[i].valid_duration);
	}

	printf("Testing working rates...\n");
	for (i = 0; i < ARRAY_SIZE(chan_freq); i++) {
		if (chan1 & chan2 & BIT(i))
			test_data_rates_work(args.if1, args.if2, chan_freq[i], pkt_params, 4);
	}
	for (i = 0; i < ARRAY_SIZE(chan_freq); i++) {
		if (chan1 & chan2 & BIT(i))
			test_data_rates_work(args.if2, args.if1, chan_freq[i], pkt_params, 4);
	}

	return 0;
}