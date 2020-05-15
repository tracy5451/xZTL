#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <xapp.h>
#include <sys/queue.h>
#include <xapp-mempool.h>

extern struct xapp_core core;

struct xapp_prometheus_stats {
    uint64_t written_bytes;
    uint64_t read_bytes;
    uint64_t io_count;
    uint64_t user_write_bytes;
    uint64_t zns_write_bytes;

    /* Flushing thread Timing */
    struct timespec ts_s;
    struct timespec ts_e;
    uint64_t us_s;
    uint64_t us_e;

    /* Flushing latency thread Timing */
    struct timespec ts_l_s;
    struct timespec ts_l_e;
    uint64_t us_l_s;
    uint64_t us_l_e;
};

static pthread_t th_flush;
static struct xapp_prometheus_stats pr_stats;
static uint8_t xapp_flush_l_running, xapp_flush_running;

/* Latency queue */

#define MAX_LATENCY_ENTS 8192

struct latency_entry {
    uint64_t usec;
    void *mp_entry;
    STAILQ_ENTRY (latency_entry) entry;
};

static pthread_t latency_tid;
static pthread_spinlock_t lat_spin;
STAILQ_HEAD (latency_head, latency_entry) lat_head;

static void xapp_prometheus_file_int64 (const char *fname, uint64_t val)
{
    FILE *fp;

    fp = fopen(fname, "w+");
    if (fp) {
	fprintf(fp, "%lu", val);
	fclose(fp);
    }
}

static void xapp_prometheus_file_double (const char *fname, double val)
{
    FILE *fp;

    fp = fopen(fname, "w+");
    if (fp) {
	fprintf(fp, "%.6lf", val);
	fclose(fp);
    }
}

static void xapp_prometheus_reset (void)
{
    uint64_t write, read, io;
    double thput_w, thput_r, thput, wa;

    GET_MICROSECONDS(pr_stats.us_s, pr_stats.ts_s);

    write = pr_stats.written_bytes;
    read  = pr_stats.read_bytes;
    io    = pr_stats.io_count;

    xapp_atomic_int64_update (&pr_stats.written_bytes, 0);
    xapp_atomic_int64_update (&pr_stats.read_bytes, 0);
    xapp_atomic_int64_update (&pr_stats.io_count, 0);

    thput_w = (double) write / (double) 1048576;
    thput_r = (double) read / (double) 1048576;
    thput   = thput_w + thput_r;

    if (pr_stats.user_write_bytes) {
	wa  = (double) pr_stats.zns_write_bytes /
	      (double) pr_stats.user_write_bytes;
    } else {
	wa  = 1;
    }

    xapp_prometheus_file_double("/tmp/ztl_prometheus_thput_w", thput_w);
    xapp_prometheus_file_double("/tmp/ztl_prometheus_thput_r", thput_r);
    xapp_prometheus_file_double("/tmp/ztl_prometheus_thput", thput);
    xapp_prometheus_file_int64 ("/tmp/ztl_prometheus_iops", io);
    xapp_prometheus_file_double("/tmp/ztl_prometheus_wamp_ztl", wa);
}

void *xapp_prometheus_flush (void *arg)
{
    GET_MICROSECONDS(pr_stats.us_s, pr_stats.ts_s);

    xapp_flush_running++;
    while (xapp_flush_running) {
	usleep(1);
	GET_MICROSECONDS(pr_stats.us_e, pr_stats.ts_e);

	if (pr_stats.us_e - pr_stats.us_s >= 1000000) {
	    xapp_prometheus_reset();
	}
    }

    while (pr_stats.us_e - pr_stats.us_s < 1000000) {
	GET_MICROSECONDS(pr_stats.us_e, pr_stats.ts_e);
    }
    xapp_prometheus_reset();

    return NULL;
}

void xapp_prometheus_add_io (struct xapp_io_mcmd *cmd)
{
    uint32_t nsec = 0, i;

    for (i = 0; i < cmd->naddr; i++)
	nsec += cmd->nsec[i];

    switch (cmd->opcode) {
	case XAPP_ZONE_APPEND:
	case XAPP_CMD_WRITE:
	    xapp_atomic_int64_update (&pr_stats.written_bytes,
				      pr_stats.written_bytes +
				      (nsec * core.media->geo.nbytes));
	    break;
	case XAPP_CMD_READ:
	    xapp_atomic_int64_update (&pr_stats.read_bytes,
				      pr_stats.read_bytes +
				      (nsec * core.media->geo.nbytes));
	    break;
	default:
	    return;
    }

    xapp_atomic_int64_update (&pr_stats.io_count, pr_stats.io_count + 1);
}

void xapp_prometheus_add_wa (uint64_t user_writes, uint64_t zns_writes)
{
    xapp_atomic_int64_update (&pr_stats.user_write_bytes, user_writes);
    xapp_atomic_int64_update (&pr_stats.zns_write_bytes, zns_writes);
}

static void xapp_prometheus_flush_latency (void)
{
    FILE *fp;
    int dequeued = 0;
    struct latency_entry *ent;
    uint64_t lat;

    GET_MICROSECONDS(pr_stats.us_l_s, pr_stats.ts_l_s);

    fp = fopen("/tmp/ztl_prometheus_read_lat", "w");
    if (fp) {
	while (!STAILQ_EMPTY(&lat_head) || dequeued < MAX_LATENCY_ENTS) {

	    pthread_spin_lock (&lat_spin);

	    ent = STAILQ_FIRST(&lat_head);
	    if (ent) {
		lat = ent->usec;
		STAILQ_REMOVE_HEAD (&lat_head, entry);
		xapp_mempool_put (ent->mp_entry, XAPP_PROMETHEUS_LAT, 0);

		pthread_spin_unlock (&lat_spin);

		fprintf(fp, "%lu\n", lat);
//		if (ent->usec > 10000)
//		    printf("TOO HIGH LATENCY (flush): %lu\n", ent->usec);
	    } else {
		pthread_spin_unlock (&lat_spin);
	    }
	    dequeued++;

	}

	fclose(fp);
    }
}

void *xapp_prometheus_latency_th (void *arg)
{
    GET_MICROSECONDS(pr_stats.us_l_s, pr_stats.ts_l_s);

    xapp_flush_l_running++;
    while (xapp_flush_l_running) {
	usleep(1);
	GET_MICROSECONDS(pr_stats.us_l_e, pr_stats.ts_l_e);

	if ((xapp_mempool_left (XAPP_PROMETHEUS_LAT, 0) < 512) ||
	    (pr_stats.us_l_e - pr_stats.us_l_s >= 1000000)) {
	    //printf ("left %d\n",  xapp_mempool_left (XAPP_PROMETHEUS_LAT, 0));
	    xapp_prometheus_flush_latency();
	}
    }

    xapp_prometheus_flush_latency();

    return NULL;

}

void xapp_prometheus_add_read_latency (uint64_t usec)
{
    struct xapp_mp_entry *mp_ent;
    struct latency_entry *ent;

    pthread_spin_lock (&lat_spin);

    /* Discard latency if queue is full */
    if (xapp_mempool_left (XAPP_PROMETHEUS_LAT, 0) == 0)
	return;

    mp_ent = xapp_mempool_get (XAPP_PROMETHEUS_LAT, 0);
    ent = (struct latency_entry *) mp_ent->opaque;
    ent->mp_entry = mp_ent;
    ent->usec = usec;

    STAILQ_INSERT_TAIL (&lat_head, ent, entry);

    pthread_spin_unlock (&lat_spin);

//    if (usec > 10000)
//	printf("TOO HIGH LATENCY (add): %lu\n", usec);
}

void xapp_prometheus_exit (void)
{
    xapp_flush_running = 0;
    xapp_flush_l_running = 0;
    pthread_join(th_flush, NULL);
    pthread_join(latency_tid, NULL);
    xapp_mempool_destroy (XAPP_PROMETHEUS_LAT, 0);
    pthread_spin_destroy (&lat_spin);
}

int xapp_prometheus_init (void) {
    int ret;

    memset(&pr_stats, 0, sizeof(struct xapp_prometheus_stats));

    /* Create layency memory pool and queue */
    STAILQ_INIT (&lat_head);

    if (pthread_spin_init (&lat_spin, 0))
	return -1;

    ret = xapp_mempool_create (XAPP_PROMETHEUS_LAT, 0, MAX_LATENCY_ENTS,
			       sizeof(struct latency_entry), NULL, NULL);
    if (ret) {
	log_err("xapp-prometheus: Latency memory pool not started.");
	goto SPIN;
    }

    xapp_flush_l_running = 0;
    if (pthread_create(&latency_tid, NULL, xapp_prometheus_latency_th, NULL)) {
	log_err("xapp-prometheus: Flushing latency thread not started.");
	goto MP;
    }

    xapp_flush_running = 0;
    if (pthread_create(&th_flush, NULL, xapp_prometheus_flush, NULL)) {
	log_err("xapp-prometheus: Flushing thread not started.");
	goto LAT_TH;
    }

    while (!xapp_flush_running || !xapp_flush_l_running) {}

    return 0;

LAT_TH:
    while (!xapp_flush_l_running) {}
    xapp_flush_l_running = 0;
    pthread_join(th_flush, NULL);
MP:
    xapp_mempool_destroy (XAPP_PROMETHEUS_LAT, 0);
SPIN:
    pthread_spin_destroy (&lat_spin);
    return -1;
}