/* CAN logger for SocketCAN on Linux
   (C)2016 Simon Urbanek, all right reserved.
   License: BSD

   Note: hard-coded to use /candump as target directory.
   Uses simplified binary format (16-byte long records):
   timestamp (uint32_t milliseconds), can_id (uint_32), data (uint8_t[8])
   and also generates following "fake" PIDs (can_id):
   PID_START_TIME  - payload is the full timeval struct of the first timestamp
   PID_DROP        - payload is unsigned integer, count of dropped frames

   Use: cand [<interface>]

   defaults to can0, interface can be "any" to listen to all interfaces.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

struct rec {
    uint32_t ts;
    uint32_t can_id;
    char data[8];
} rec;

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#define ANYDEV "any"  /* name of interface to receive from any CAN interface */

#define PID_START_TIME 0x8000 /* payload: struct timeval */
#define PID_DROP       0x8001 /* payload: # of dropped frames */

int main(int ac, char **av) {
    FILE *f;
    time_t currtime;
    struct tm now;
    static char fname[64];
    int s;
    int rcvbuf_size = 0;
    struct sockaddr_can addr;
    char ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
    struct ifreq ifr;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    char *ifname = "can0";
    __u32 last_drcnt = 0;

    if (ac > 1) ifname = av[0];
    
    if (time(&currtime) == (time_t)-1) {
	perror("ERROR: time() failed");
	return 1;
    }
    
    localtime_r(&currtime, &now);
    
    sprintf(fname, "/candump/candump-%04d-%02d-%02d_%02d%02d%02d.bin",
	    now.tm_year + 1900,
	    now.tm_mon + 1,
	    now.tm_mday,
	    now.tm_hour,
	    now.tm_min,
	    now.tm_sec);

    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
	perror("ERROR: socket");
	return 1;
    }

    addr.can_family = AF_CAN;
    memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
    strcpy(ifr.ifr_name, ifname);

    if (strcmp(ANYDEV, ifr.ifr_name)) {
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
	    perror("SIOCGIFINDEX");
	    exit(1);
	}
	addr.can_ifindex = ifr.ifr_ifindex;
    } else
	addr.can_ifindex = 0; /* any can interface */
    
    if (rcvbuf_size) {
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF,
		       &rcvbuf_size, sizeof(rcvbuf_size)) < 0)
	    perror("WARN: setsockopt SO_RCVBUF not supported");
    }
    {
	const int timestamp_on = 1;
	if (setsockopt(s, SOL_SOCKET, SO_TIMESTAMP,
		       &timestamp_on, sizeof(timestamp_on)) < 0)
	    perror("WARN: setsockopt SO_TIMESTAMP not supported");
    }
    {
	const int dropmonitor_on = 1;
	if (setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL,
		       &dropmonitor_on, sizeof(dropmonitor_on)) < 0)
	    perror("WARN: setsockopt SO_RXQ_OVFL not supported");
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
	perror("ERROR: cannot bind");
	return 1;
    }

    f = fopen(fname, "wb");
    if (!f) {
	fprintf(stderr, "ERROR: cannot create '%s'", fname);
	perror("");
	return 1;
    }

    struct can_frame frame;
    int first = 1;
    
    iov.iov_base = &frame;
    msg.msg_name = &addr;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &ctrlmsg;

    int last_tv = 0;
    
    while (1) {
	iov.iov_len = sizeof(frame);
	msg.msg_namelen = sizeof(addr);
	msg.msg_controllen = sizeof(ctrlmsg);
	msg.msg_flags = 0;

	struct timeval tv;
	int nbytes = recvmsg(s, &msg, 0);
	if (nbytes < 0) {
	    perror("read");
	    break;
	}

	for (cmsg = CMSG_FIRSTHDR(&msg);
	     cmsg && (cmsg->cmsg_level == SOL_SOCKET);
	     cmsg = CMSG_NXTHDR(&msg,cmsg)) {
	    if (cmsg->cmsg_type == SO_TIMESTAMP) {
		tv = *(struct timeval *)CMSG_DATA(cmsg);
		if (first) { /* record the first full timestamp */
		    rec.ts = (tv.tv_usec / 1000) + (tv.tv_sec * 1000);
		    rec.can_id = PID_START_TIME;
		    memcpy(rec.data, &tv, 8);
		    fwrite(&rec, sizeof(rec), 1, f);
		    first = 0;
		}
	    } else if (cmsg->cmsg_type == SO_RXQ_OVFL) {
		__u32 dropcnt = *(__u32 *)CMSG_DATA(cmsg);
		if (last_drcnt != dropcnt) {
		  rec.ts = (tv.tv_usec / 1000) + (tv.tv_sec * 1000);
		  rec.can_id = PID_DROP;
		  memset(rec.data, 0, 8);
		  ((__u32*)rec.data)[0] = dropcnt - last_drcnt;
		  fwrite(&rec, sizeof(rec), 1, f);
		  fflush(f);
		  last_drcnt = last_drcnt;
		}
	    }		
	}

	rec.ts  = (tv.tv_usec / 1000) + (tv.tv_sec * 1000);
	rec.can_id = frame.can_id;
	memcpy(rec.data, frame.data, 8);
	fwrite(&rec, sizeof(rec), 1, f);
	if (last_tv && tv.tv_sec - last_tv > 2) {
	  last_tv = tv.tv_sec;
	  fflush(f);
	}
    }
    return 0;
}
